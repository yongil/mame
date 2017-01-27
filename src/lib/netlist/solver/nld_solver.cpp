// license:GPL-2.0+
// copyright-holders:Couriersud
/*
 * nld_solver.c
 *
 */

/* Commented out for now. Relatively low number of terminals / nets make
 * the vectorizations fast-math enables pretty expensive
 */

#if 0
#pragma GCC optimize "fast-math"
#pragma GCC optimize "strict-aliasing"
#pragma GCC optimize "tree-vectorize"
#pragma GCC optimize "tree-vectorizer-verbose=7"
#pragma GCC optimize "opt-info-vec"
#pragma GCC optimize "opt-info-vec-missed"
//#pragma GCC optimize "tree-parallelize-loops=4"
#pragma GCC optimize "unroll-loops"
#pragma GCC optimize "unswitch-loops"
#pragma GCC optimize "variable-expansion-in-unroller"
#pragma GCC optimize "unsafe-loop-optimizations"
#pragma GCC optimize "vect-cost-model"
#pragma GCC optimize "variable-expansion-in-unroller"
#pragma GCC optimize "tree-loop-if-convert-stores"
#pragma GCC optimize "tree-loop-distribution"
#pragma GCC optimize "tree-loop-im"
#pragma GCC optimize "tree-loop-ivcanon"
#pragma GCC optimize "ivopts"
#endif

#include <algorithm>
#include <cmath>  // <<= needed by windows build

#include "nl_lists.h"

#if HAS_OPENMP
#include "omp.h"
#endif

#include "nld_solver.h"
#include "nld_matrix_solver.h"

#if 1
#include "nld_ms_direct.h"
#include "nld_ms_gcr.h"
#else
#include "nld_ms_direct_lu.h"
#endif
#include "nld_ms_w.h"
#include "nld_ms_sm.h"
#include "nld_ms_direct1.h"
#include "nld_ms_direct2.h"
#include "nld_ms_sor.h"
#include "nld_ms_sor_mat.h"
#include "nld_ms_gmres.h"

namespace netlist
{
	namespace devices
	{



// ----------------------------------------------------------------------------------------
// solver
// ----------------------------------------------------------------------------------------

NETLIB_RESET(solver)
{
	for (std::size_t i = 0; i < m_mat_solvers.size(); i++)
		m_mat_solvers[i]->do_reset();
}

void NETLIB_NAME(solver)::stop()
{
	for (std::size_t i = 0; i < m_mat_solvers.size(); i++)
		m_mat_solvers[i]->log_stats();
}

NETLIB_NAME(solver)::~NETLIB_NAME(solver)()
{
}

NETLIB_UPDATE(solver)
{
	if (m_params.m_dynamic_ts)
		return;

	/* force solving during start up if there are no time-step devices */
	/* FIXME: Needs a more elegant solution */
	bool force_solve = (netlist().time() < netlist_time::from_double(2 * m_params.m_max_timestep));

#if HAS_OPENMP && USE_OPENMP
	const std::size_t t_cnt = m_mat_solvers.size();
	if (m_parallel())
	{
		//omp_set_num_threads(3);
		//omp_set_dynamic(0);
		#pragma omp parallel
		{
			#pragma omp for
			for (int i = 0; i <  t_cnt; i++)
				if (m_mat_solvers[i]->has_timestep_devices() || force_solve)
				{
					// Ignore return value
					ATTR_UNUSED const netlist_time ts = m_mat_solvers[i]->solve();
				}
		}

		for (auto & solver : m_mat_solvers)
			if (solver->has_timestep_devices() || force_solve)
				solver->update_inputs();
	}
	else
		for (int i = 0; i < t_cnt; i++)
			if (m_mat_solvers[i]->has_timestep_devices() || force_solve)
			{
				// Ignore return value
				ATTR_UNUSED const netlist_time ts = m_mat_solvers[i]->solve();
				solver->update_inputs();
			}
#else
	for (auto & solver : m_mat_solvers)
		if (solver->has_timestep_devices() || force_solve)
		{
			// Ignore return value
			ATTR_UNUSED const netlist_time ts = solver->solve();
			solver->update_inputs();
		}
#endif

	/* step circuit */
	if (!m_Q_step.net().is_queued())
	{
		m_Q_step.net().toggle_new_Q();
		m_Q_step.net().push_to_queue(netlist_time::from_double(m_params.m_max_timestep));
	}
}

template <class C>
std::unique_ptr<matrix_solver_t> create_it(netlist_t &nl, pstring name, solver_parameters_t &params, std::size_t size)
{
	typedef C solver;
	return plib::make_unique<solver>(nl, name, &params, size);
}

template <std::size_t m_N, std::size_t storage_N>
std::unique_ptr<matrix_solver_t> NETLIB_NAME(solver)::create_solver(std::size_t size, const bool use_specific)
{
	pstring solvername = plib::pfmt("Solver_{1}")(m_mat_solvers.size());
	if (use_specific && m_N == 1)
		return plib::make_unique<matrix_solver_direct1_t>(netlist(), solvername, &m_params);
	else if (use_specific && m_N == 2)
		return plib::make_unique<matrix_solver_direct2_t>(netlist(), solvername, &m_params);
	else
	{
		if (pstring("SOR_MAT").equals(m_method()))
		{
			return create_it<matrix_solver_SOR_mat_t<m_N, storage_N>>(netlist(), solvername, m_params, size);
			//typedef matrix_solver_SOR_mat_t<m_N,storage_N> solver_sor_mat;
			//return plib::make_unique<solver_sor_mat>(netlist(), solvername, &m_params, size);
		}
		else if (pstring("MAT_CR").equals(m_method()))
		{
			typedef matrix_solver_GCR_t<m_N,storage_N> solver_mat;
			return plib::make_unique<solver_mat>(netlist(), solvername, &m_params, size);
		}
		else if (pstring("MAT").equals(m_method()))
		{
			typedef matrix_solver_direct_t<m_N,storage_N> solver_mat;
			return plib::make_unique<solver_mat>(netlist(), solvername, &m_params, size);
		}
		else if (pstring("SM").equals(m_method()))
		{
			/* Sherman-Morrison Formula */
			typedef matrix_solver_sm_t<m_N,storage_N> solver_mat;
			return plib::make_unique<solver_mat>(netlist(), solvername, &m_params, size);
		}
		else if (pstring("W").equals(m_method()))
		{
			/* Woodbury Formula */
			typedef matrix_solver_w_t<m_N,storage_N> solver_mat;
			return plib::make_unique<solver_mat>(netlist(), solvername, &m_params, size);
		}
		else if (pstring("SOR").equals(m_method()))
		{
			typedef matrix_solver_SOR_t<m_N,storage_N> solver_GS;
			return plib::make_unique<solver_GS>(netlist(), solvername, &m_params, size);
		}
		else if (pstring("GMRES").equals(m_method()))
		{
			typedef matrix_solver_GMRES_t<m_N,storage_N> solver_GMRES;
			return plib::make_unique<solver_GMRES>(netlist(), solvername, &m_params, size);
		}
		else
		{
			log().fatal(MF_1_UNKNOWN_SOLVER_TYPE, m_method());
			return nullptr;
		}
	}
}

struct net_splitter
{

	bool already_processed(analog_net_t *n)
	{
		if (n->isRailNet())
			return true;
		for (auto & grp : groups)
			if (plib::container::contains(grp, n))
				return true;
		return false;
	}

	void process_net(analog_net_t *n)
	{
		if (n->num_cons() == 0)
			return;
		/* add the net */
		groups.back().push_back(n);
		for (auto &p : n->m_core_terms)
		{
			if (p->is_type(terminal_t::TERMINAL))
			{
				terminal_t *pt = static_cast<terminal_t *>(p);
				analog_net_t *other_net = &pt->m_otherterm->net();
				if (!already_processed(other_net))
					process_net(other_net);
			}
		}
	}

	void run(netlist_t &netlist)
	{
		for (auto & net : netlist.m_nets)
		{
			netlist.log().debug("processing {1}\n", net->name());
			if (!net->isRailNet() && net->num_cons() > 0)
			{
				netlist.log().debug("   ==> not a rail net\n");
				/* Must be an analog net */
				analog_net_t *n = static_cast<analog_net_t *>(net.get());
				if (!already_processed(n))
				{
					groups.push_back(analog_net_t::list_t());
					process_net(n);
				}
			}
		}
	}

	std::vector<analog_net_t::list_t> groups;
};

void NETLIB_NAME(solver)::post_start()
{
	const bool use_specific = true;

	m_params.m_pivot = m_pivot();
	m_params.m_accuracy = m_accuracy();
	/* FIXME: Throw when negative */
	m_params.m_gs_loops = static_cast<unsigned>(m_gs_loops());
	m_params.m_nr_loops = static_cast<unsigned>(m_nr_loops());
	m_params.m_nr_recalc_delay = netlist_time::from_double(m_nr_recalc_delay());
	m_params.m_dynamic_lte = m_dynamic_lte();
	m_params.m_gs_sor = m_gs_sor();

	m_params.m_min_timestep = m_dynamic_min_ts();
	m_params.m_dynamic_ts = (m_dynamic_ts() == 1 ? true : false);
	m_params.m_max_timestep = netlist_time::from_double(1.0 / m_freq()).as_double();

	if (m_params.m_dynamic_ts)
	{
		m_params.m_max_timestep *= 1;//NL_FCONST(1000.0);
	}
	else
	{
		m_params.m_min_timestep = m_params.m_max_timestep;
	}

	//m_params.m_max_timestep = std::max(m_params.m_max_timestep, m_params.m_max_timestep::)

	// Override log statistics
	pstring p = plib::util::environment("NL_STATS", "");
	if (p != "")
		m_params.m_log_stats = p.as_long();
	else
		m_params.m_log_stats = m_log_stats();

	log().verbose("Scanning net groups ...");
	// determine net groups

	net_splitter splitter;

	splitter.run(netlist());

	// setup the solvers
	log().verbose("Found {1} net groups in {2} nets\n", splitter.groups.size(), netlist().m_nets.size());
	for (auto & grp : splitter.groups)
	{
		std::unique_ptr<matrix_solver_t> ms;
		std::size_t net_count = grp.size();

		switch (net_count)
		{
#if 1
			case 1:
				ms = create_solver<1,1>(1, false);
				break;
			case 2:
				ms = create_solver<2,2>(2, false);
				break;
			case 3:
				ms = create_solver<3,3>(3, use_specific);
				break;
			case 4:
				ms = create_solver<4,4>(4, use_specific);
				break;
			case 5:
				ms = create_solver<5,5>(5, use_specific);
				break;
			case 6:
				ms = create_solver<6,6>(6, use_specific);
				break;
			case 7:
				ms = create_solver<7,7>(7, use_specific);
				break;
			case 8:
				ms = create_solver<8,8>(8, use_specific);
				break;
			case 9:
				ms = create_solver<9,9>(9, use_specific);
				break;
			case 10:
				ms = create_solver<10,10>(10, use_specific);
				break;
			case 11:
				ms = create_solver<11,11>(11, use_specific);
				break;
			case 12:
				ms = create_solver<12,12>(12, use_specific);
				break;
			case 15:
				ms = create_solver<15,15>(15, use_specific);
				break;
			case 31:
				ms = create_solver<31,31>(31, use_specific);
				break;
			case 35:
				ms = create_solver<31,31>(31, use_specific);
				break;
			case 49:
				ms = create_solver<49,49>(49, use_specific);
				break;
#if 0
			case 87:
				ms = create_solver<87,87>(87, use_specific);
				break;
#endif
#endif
			default:
				log().warning(MW_1_NO_SPECIFIC_SOLVER, net_count);
				if (net_count <= 8)
				{
					ms = create_solver<0, 8>(net_count, use_specific);
				}
				else if (net_count <= 16)
				{
					ms = create_solver<0,16>(net_count, use_specific);
				}
				else if (net_count <= 32)
				{
					ms = create_solver<0,32>(net_count, use_specific);
				}
				else
					if (net_count <= 64)
				{
					ms = create_solver<0,64>(net_count, use_specific);
				}
				else
					if (net_count <= 128)
				{
					ms = create_solver<0,128>(net_count, use_specific);
				}
				else
				{
					log().fatal(MF_1_NETGROUP_SIZE_EXCEEDED_1, 128);
					ms = nullptr; /* tease compilers */
				}

				break;
		}

		// FIXME ...
		ms->set_delegate_pointer();
		ms->setup(grp);

		log().verbose("Solver {1}", ms->name());
		log().verbose("       ==> {2} nets", grp.size());
		log().verbose("       has {1} elements", ms->has_dynamic_devices() ? "dynamic" : "no dynamic");
		log().verbose("       has {1} elements", ms->has_timestep_devices() ? "timestep" : "no timestep");
		for (auto &n : grp)
		{
			log().verbose("Net {1}", n->name());
			for (const auto &pcore : n->m_core_terms)
			{
				log().verbose("   {1}", pcore->name());
			}
		}

		m_mat_solvers.push_back(std::move(ms));
	}
}

void NETLIB_NAME(solver)::create_solver_code(std::map<pstring, pstring> &mp)
{
	for (auto & s : m_mat_solvers)
	{
		auto r = s->create_solver_code();
		mp[r.first] = r.second; // automatically overwrites identical names
	}
}

	NETLIB_DEVICE_IMPL(solver)

	} //namespace devices
} // namespace netlist
