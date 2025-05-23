#ifndef P3_MAIN_IMPL_HPP
#define P3_MAIN_IMPL_HPP

#include "p3_functions.hpp" // for ETI only but harmless for GPU
#include "physics/share/physics_functions.hpp" // also for ETI not on GPUs
#include "physics/share/physics_saturation_impl.hpp"

#include "ekat/kokkos/ekat_subview_utils.hpp"

namespace scream {
namespace p3 {

/*
 * Implementation of p3 main function. Clients should NOT #include
 * this file, #include p3_functions.hpp instead.
 */

template <typename S, typename D>
KOKKOS_FUNCTION
void Functions<S,D>
::p3_main_init(
  const MemberType& team,
  const Int& nk_pack,
  const uview_1d<const Spack>& cld_frac_i,
  const uview_1d<const Spack>& cld_frac_l,
  const uview_1d<const Spack>& cld_frac_r,
  const uview_1d<const Spack>& inv_exner,
  const uview_1d<const Spack>& th_atm,
  const uview_1d<const Spack>& dz,
  const uview_1d<Spack>& diag_equiv_reflectivity,
  const uview_1d<Spack>& ze_ice,
  const uview_1d<Spack>& ze_rain,
  const uview_1d<Spack>& diag_eff_radius_qc,
  const uview_1d<Spack>& diag_eff_radius_qi,
  const uview_1d<Spack>& diag_eff_radius_qr,
  const uview_1d<Spack>& inv_cld_frac_i,
  const uview_1d<Spack>& inv_cld_frac_l,
  const uview_1d<Spack>& inv_cld_frac_r,
  const uview_1d<Spack>& exner,
  const uview_1d<Spack>& T_atm,
  const uview_1d<Spack>& qv,
  const uview_1d<Spack>& inv_dz,
  Scalar& precip_liq_surf,
  Scalar& precip_ice_surf,
  view_1d_ptr_array<Spack, 36>& zero_init)
{
  precip_liq_surf = 0;
  precip_ice_surf = 0;

  Kokkos::parallel_for(
    Kokkos::TeamVectorRange(team, nk_pack), [&] (Int k) {

    diag_equiv_reflectivity(k)           = -99;
    ze_ice(k)            = 1.e-22;
    ze_rain(k)           = 1.e-22;
    diag_eff_radius_qc(k)         = 10.e-6;
    diag_eff_radius_qi(k)         = 25.e-6;
    diag_eff_radius_qr(k)         = 500.e-6;
    inv_cld_frac_i(k)    = 1 / cld_frac_i(k);
    inv_cld_frac_l(k)    = 1 / cld_frac_l(k);
    inv_cld_frac_r(k)    = 1 / cld_frac_r(k);
    exner(k)         = 1 / inv_exner(k);
    T_atm(k)                 = th_atm(k) * exner(k);
    qv(k)                = max(qv(k), 0);
    inv_dz(k)            = 1 / dz(k);

    for (size_t j = 0; j < zero_init.size(); ++j) {
      (*zero_init[j])(k) = 0;
    }
  });
  team.team_barrier();
}

template <typename S, typename D>
Int Functions<S,D>
::p3_main_internal(
  const P3Runtime& runtime_options,
  const P3PrognosticState& prognostic_state,
  const P3DiagnosticInputs& diagnostic_inputs,
  const P3DiagnosticOutputs& diagnostic_outputs,
  const P3Infrastructure& infrastructure,
  const P3HistoryOnly& history_only,
  const P3LookupTables& lookup_tables,
  const WorkspaceManager& workspace_mgr,
  Int nj,
  Int nk)
{
  using ExeSpace = typename KT::ExeSpace;
  using ScratchViewType = Kokkos::View<bool*, typename ExeSpace::scratch_memory_space>;

  const Int nk_pack = ekat::npack<Spack>(nk);
  const auto scratch_size = ScratchViewType::shmem_size(2);
  const auto policy = ekat::ExeSpaceUtils<ExeSpace>::get_default_team_policy(nj, nk_pack).set_scratch_size(0, Kokkos::PerTeam(scratch_size));

  // load constants into local vars
  const     Scalar inv_dt          = 1 / infrastructure.dt;
  constexpr Int    kdir         = -1;
  const     Int    ktop         = kdir == -1 ? 0    : nk-1;
  const     Int    kbot         = kdir == -1 ? nk-1 : 0;
  constexpr bool   debug_ABORT  = false;

  const bool do_ice_production = runtime_options.do_ice_production;

  // we do not want to measure init stuff
  auto start = std::chrono::steady_clock::now();

  // p3_main loop
  Kokkos::parallel_for(
    "p3 main loop",
    policy,
    KOKKOS_LAMBDA(const MemberType& team) {

    const Int i = team.league_rank();

    auto workspace = workspace_mgr.get_workspace(team);

    //
    // Get temporary workspaces needed for p3
    //
    uview_1d<Spack>
      mu_r,   // shape parameter of rain
      T_atm,      // temperature at the beginning of the microphysics step [K]

      // 2D size distribution and fallspeed parameters
      lamr, logn0r, nu, cdist, cdist1, cdistr,

      // Variables needed for in-cloud calculations
      inv_cld_frac_i, inv_cld_frac_l, inv_cld_frac_r, // Inverse cloud fractions (1/cld)
      qc_incld, qr_incld, qi_incld, qm_incld, // In cloud mass-mixing ratios
      nc_incld, nr_incld, ni_incld, bm_incld, // In cloud number concentrations

      // Other
      inv_dz, inv_rho, ze_ice, ze_rain, prec, rho,
      rhofacr, rhofaci, acn, qv_sat_l, qv_sat_i, sup, qv_supersat_i,
      tmparr1, exner, diag_vm_qi, diag_diam_qi, pratot, prctot,

      // p3_tend_out, may not need these
      qtend_ignore, ntend_ignore,

      // Variables still used in F90 but removed from C++ interface
      mu_c, lamc, qr_evap_tend;

    workspace.template take_many_and_reset<43>(
      {
        "mu_r", "T_atm", "lamr", "logn0r", "nu", "cdist", "cdist1", "cdistr",
        "inv_cld_frac_i", "inv_cld_frac_l", "inv_cld_frac_r", "qc_incld", "qr_incld", "qi_incld", "qm_incld",
        "nc_incld", "nr_incld", "ni_incld", "bm_incld",
        "inv_dz", "inv_rho", "ze_ice", "ze_rain", "prec", "rho",
        "rhofacr", "rhofaci", "acn", "qv_sat_l", "qv_sat_i", "sup", "qv_supersat_i",
        "tmparr1", "exner", "diag_vm_qi", "diag_diam_qi",
        "pratot", "prctot", "qtend_ignore", "ntend_ignore",
        "mu_c", "lamc", "qr_evap_tend"
      },
      {
        &mu_r, &T_atm, &lamr, &logn0r, &nu, &cdist, &cdist1, &cdistr,
        &inv_cld_frac_i, &inv_cld_frac_l, &inv_cld_frac_r, &qc_incld, &qr_incld, &qi_incld, &qm_incld,
        &nc_incld, &nr_incld, &ni_incld, &bm_incld,
        &inv_dz, &inv_rho, &ze_ice, &ze_rain, &prec, &rho,
        &rhofacr, &rhofaci, &acn, &qv_sat_l, &qv_sat_i, &sup, &qv_supersat_i,
        &tmparr1, &exner, &diag_vm_qi, &diag_diam_qi,
        &pratot, &prctot, &qtend_ignore, &ntend_ignore,
        &mu_c, &lamc, &qr_evap_tend
      });

    // Get single-column subviews of all inputs, shouldn't need any i-indexing
    // after this.
    const auto opres               = ekat::subview(diagnostic_inputs.pres, i);
    const auto odz                 = ekat::subview(diagnostic_inputs.dz, i);
    const auto onc_nuceat_tend     = ekat::subview(diagnostic_inputs.nc_nuceat_tend, i);
    const auto onccn_prescribed    = ekat::subview(diagnostic_inputs.nccn, i);
    const auto oni_activated       = ekat::subview(diagnostic_inputs.ni_activated, i);
    const auto oinv_qc_relvar      = ekat::subview(diagnostic_inputs.inv_qc_relvar, i);
    const auto odpres              = ekat::subview(diagnostic_inputs.dpres, i);
    const auto oinv_exner          = ekat::subview(diagnostic_inputs.inv_exner, i);
    const auto ocld_frac_i         = ekat::subview(diagnostic_inputs.cld_frac_i, i);
    const auto ocld_frac_l         = ekat::subview(diagnostic_inputs.cld_frac_l, i);
    const auto ocld_frac_r         = ekat::subview(diagnostic_inputs.cld_frac_r, i);
    const auto ocol_location       = ekat::subview(infrastructure.col_location, i);
    const auto oqc                 = ekat::subview(prognostic_state.qc, i);
    const auto onc                 = ekat::subview(prognostic_state.nc, i);
    const auto oqr                 = ekat::subview(prognostic_state.qr, i);
    const auto onr                 = ekat::subview(prognostic_state.nr, i);
    const auto oqi                 = ekat::subview(prognostic_state.qi, i);
    const auto oqm                 = ekat::subview(prognostic_state.qm, i);
    const auto oni                 = ekat::subview(prognostic_state.ni, i);
    const auto obm                 = ekat::subview(prognostic_state.bm, i);
    const auto oqv                 = ekat::subview(prognostic_state.qv, i);
    const auto oth                 = ekat::subview(prognostic_state.th, i);
    const auto odiag_eff_radius_qc = ekat::subview(diagnostic_outputs.diag_eff_radius_qc, i);
    const auto odiag_eff_radius_qi = ekat::subview(diagnostic_outputs.diag_eff_radius_qi, i);
    const auto odiag_eff_radius_qr = ekat::subview(diagnostic_outputs.diag_eff_radius_qr, i);
    const auto oqv2qi_depos_tend   = ekat::subview(diagnostic_outputs.qv2qi_depos_tend, i);
    const auto orho_qi             = ekat::subview(diagnostic_outputs.rho_qi, i);
    const auto oprecip_liq_flux    = ekat::subview(diagnostic_outputs.precip_liq_flux, i);
    const auto oprecip_ice_flux    = ekat::subview(diagnostic_outputs.precip_ice_flux, i);
    const auto oprecip_total_tend  = ekat::subview(diagnostic_outputs.precip_total_tend, i);
    const auto onevapr             = ekat::subview(diagnostic_outputs.nevapr, i);
    const auto odiag_equiv_refl    = ekat::subview(diagnostic_outputs.diag_equiv_reflectivity, i);
    const auto oliq_ice_exchange   = ekat::subview(history_only.liq_ice_exchange, i);
    const auto ovap_liq_exchange   = ekat::subview(history_only.vap_liq_exchange, i);
    const auto ovap_ice_exchange   = ekat::subview(history_only.vap_ice_exchange, i);
    const auto oqr2qv_evap         = ekat::subview(history_only.qr2qv_evap, i);
    const auto oqi2qv_sublim       = ekat::subview(history_only.qi2qv_sublim, i);
    const auto oqc2qr_accret       = ekat::subview(history_only.qc2qr_accret,i);
    const auto oqc2qr_autoconv     = ekat::subview(history_only.qc2qr_autoconv,i);
    const auto oqv2qi_vapdep       = ekat::subview(history_only.qv2qi_vapdep,i);
    const auto oqc2qi_berg         = ekat::subview(history_only.qc2qi_berg,i);
    const auto oqc2qr_ice_shed     = ekat::subview(history_only.qc2qr_ice_shed,i);
    const auto oqc2qi_collect      = ekat::subview(history_only.qc2qi_collect,i);
    const auto oqr2qi_collect      = ekat::subview(history_only.qr2qi_collect,i);
    const auto oqc2qi_hetero_freeze = ekat::subview(history_only.qc2qi_hetero_freeze,i);
    const auto oqr2qi_immers_freeze = ekat::subview(history_only.qr2qi_immers_freeze,i);
    const auto oqi2qr_melt         = ekat::subview(history_only.qi2qr_melt,i);
    const auto oqr_sed             = ekat::subview(history_only.qr_sed, i);
    const auto oqc_sed             = ekat::subview(history_only.qc_sed, i);
    const auto oqi_sed             = ekat::subview(history_only.qi_sed, i);
    const auto oqv_prev            = ekat::subview(diagnostic_inputs.qv_prev, i);
    const auto ot_prev             = ekat::subview(diagnostic_inputs.t_prev, i);

    // Inputs for the heteogeneous freezing
    const auto ohetfrz_immersion_nucleation_tend  = ekat::subview(diagnostic_inputs.hetfrz_immersion_nucleation_tend, i);
    const auto ohetfrz_contact_nucleation_tend    = ekat::subview(diagnostic_inputs.hetfrz_contact_nucleation_tend, i);
    const auto ohetfrz_deposition_nucleation_tend = ekat::subview(diagnostic_inputs.hetfrz_deposition_nucleation_tend, i);

    // Use Kokkos' scratch pad for allocating 2 bools
    // per team to determine early exits
    ScratchViewType bools(team.team_scratch(0), 2);
    bool &nucleationPossible  = bools(0);
    bool &hydrometeorsPresent = bools(1);

    view_1d_ptr_array<Spack, 36> zero_init = {
      &mu_r, &lamr, &logn0r, &nu, &cdist, &cdist1, &cdistr,
      &qc_incld, &qr_incld, &qi_incld, &qm_incld,
      &nc_incld, &nr_incld, &ni_incld, &bm_incld,
      &inv_rho, &prec, &rho, &rhofacr, &rhofaci, &acn, &qv_sat_l, &qv_sat_i, &sup, &qv_supersat_i,
      &tmparr1, &qtend_ignore, &ntend_ignore,
      &mu_c, &lamc, &orho_qi, &oqv2qi_depos_tend, &oprecip_total_tend, &onevapr, &oprecip_liq_flux, &oprecip_ice_flux
    };

    // initialize
    p3_main_init(
      team, nk_pack,
      ocld_frac_i, ocld_frac_l, ocld_frac_r, oinv_exner, oth, odz, odiag_equiv_refl,
      ze_ice, ze_rain, odiag_eff_radius_qc, odiag_eff_radius_qi, odiag_eff_radius_qr,
      inv_cld_frac_i, inv_cld_frac_l, inv_cld_frac_r, exner, T_atm, oqv, inv_dz,
      diagnostic_outputs.precip_liq_surf(i), diagnostic_outputs.precip_ice_surf(i), zero_init);

    p3_main_part1(
      team, nk, infrastructure.predictNc, infrastructure.prescribedCCN, infrastructure.dt,
      opres, odpres, odz, onc_nuceat_tend, onccn_prescribed, oinv_exner, exner, inv_cld_frac_l, inv_cld_frac_i,
      inv_cld_frac_r,
      T_atm, rho, inv_rho, qv_sat_l, qv_sat_i, qv_supersat_i, rhofacr,
      rhofaci, acn, oqv, oth, oqc, onc, oqr, onr, oqi, oni, oqm,
      obm, qc_incld, qr_incld, qi_incld, qm_incld, nc_incld, nr_incld,
      ni_incld, bm_incld, nucleationPossible, hydrometeorsPresent, runtime_options);

    // There might not be any work to do for this team
    if (!(nucleationPossible || hydrometeorsPresent)) {
      return; // this is how you do a "continue" in a kokkos lambda
    }

    // ------------------------------------------------------------------------------------------
    // main k-loop (for processes):

    p3_main_part2(
      team, nk_pack, runtime_options.max_total_ni, infrastructure.predictNc, infrastructure.prescribedCCN, infrastructure.dt, inv_dt,
      ohetfrz_immersion_nucleation_tend, ohetfrz_contact_nucleation_tend, ohetfrz_deposition_nucleation_tend,
      lookup_tables.dnu_table_vals, lookup_tables.ice_table_vals, lookup_tables.collect_table_vals, lookup_tables.revap_table_vals, opres, odpres, odz, onc_nuceat_tend, oinv_exner,
      exner, inv_cld_frac_l, inv_cld_frac_i, inv_cld_frac_r, oni_activated, oinv_qc_relvar, ocld_frac_i,
      ocld_frac_l, ocld_frac_r, oqv_prev, ot_prev, T_atm, rho, inv_rho, qv_sat_l, qv_sat_i, qv_supersat_i, rhofacr, rhofaci, acn,
      oqv, oth, oqc, onc, oqr, onr, oqi, oni, oqm, obm,
      qc_incld, qr_incld, qi_incld, qm_incld, nc_incld,
      nr_incld, ni_incld, bm_incld, mu_c, nu, lamc, cdist, cdist1, cdistr,
      mu_r, lamr, logn0r, oqv2qi_depos_tend, oprecip_total_tend, onevapr, qr_evap_tend,
      ovap_liq_exchange, ovap_ice_exchange, oliq_ice_exchange,
      oqr2qv_evap, oqi2qv_sublim, oqc2qr_accret, oqc2qr_autoconv, oqv2qi_vapdep,
      oqc2qi_berg, oqc2qr_ice_shed, oqc2qi_collect, oqr2qi_collect, oqc2qi_hetero_freeze, oqr2qi_immers_freeze, oqi2qr_melt,
      pratot, prctot, hydrometeorsPresent, nk, runtime_options);

    //NOTE: At this point, it is possible to have negative (but small) nc, nr, ni.  This is not
    //      a problem; those values get clipped to zero in the sedimentation section (if necessary).
    //      (This is not done above simply for efficiency purposes.)

    if (!hydrometeorsPresent) return;

    // -----------------------------------------------------------------------------------------
    // End of main microphysical processes section
    // =========================================================================================

    // ==========================================================================================!
    // Sedimentation:

    // Cloud sedimentation:  (adaptive substepping)

    cloud_sedimentation(
      qc_incld, rho, inv_rho, ocld_frac_l, acn, inv_dz, lookup_tables.dnu_table_vals, team, workspace,
      nk, ktop, kbot, kdir, infrastructure.dt, inv_dt, infrastructure.predictNc,
      oqc, onc, nc_incld, mu_c, lamc, oqc_sed, ntend_ignore,
      diagnostic_outputs.precip_liq_surf(i));

    // Rain sedimentation:  (adaptive substepping)
    rain_sedimentation(
      rho, inv_rho, rhofacr, ocld_frac_r, inv_dz, qr_incld, team, workspace,
      lookup_tables.vn_table_vals, lookup_tables.vm_table_vals, nk, ktop, kbot, kdir, infrastructure.dt, inv_dt, oqr,
      onr, nr_incld, mu_r, lamr, oprecip_liq_flux, oqr_sed, ntend_ignore,
      diagnostic_outputs.precip_liq_surf(i), runtime_options);

    // Ice sedimentation:  (adaptive substepping)
    ice_sedimentation(
      rho, inv_rho, rhofaci, ocld_frac_i, inv_dz, team, workspace, nk, ktop, kbot,
      kdir, infrastructure.dt, inv_dt, oqi, qi_incld, oni, ni_incld,
      oqm, qm_incld, obm, bm_incld, oqi_sed, ntend_ignore,
      lookup_tables.ice_table_vals, diagnostic_outputs.precip_ice_surf(i), runtime_options);

    // homogeneous freezing of cloud and rain
    if(do_ice_production) {
      homogeneous_freezing(T_atm, oinv_exner, team, nk, ktop, kbot, kdir, oqc,
                           onc, oqr, onr, oqi, oni, oqm, obm, oth);
    }

    //
    // final checks to ensure consistency of mass/number
    // and compute diagnostic fields for output
    //
    p3_main_part3(
      team, nk_pack, runtime_options.max_total_ni, lookup_tables.dnu_table_vals, lookup_tables.ice_table_vals, oinv_exner, ocld_frac_l, ocld_frac_r, ocld_frac_i,
      rho, inv_rho, rhofaci, oqv, oth, oqc, onc, oqr, onr, oqi, oni,
      oqm, obm, mu_c, nu, lamc, mu_r, lamr,
      ovap_liq_exchange, ze_rain, ze_ice, diag_vm_qi, odiag_eff_radius_qi, diag_diam_qi,
      orho_qi, odiag_equiv_refl, odiag_eff_radius_qc, odiag_eff_radius_qr, runtime_options);

    //
    // merge ice categories with similar properties

    //   note:  this should be relocated to above, such that the diagnostic
    //          ice properties are computed after merging

    // PMC nCat deleted nCat>1 stuff

#ifndef NDEBUG
    Kokkos::parallel_for(
      Kokkos::TeamVectorRange(team, nk_pack), [&] (Int k) {
        tmparr1(k) = oth(k) * exner(k);
    });

    check_values(oqv, tmparr1, ktop, kbot, infrastructure.it, debug_ABORT, 900,
                 team, ocol_location);
#endif
  });
  Kokkos::fence();

  auto finish = std::chrono::steady_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(finish - start);
  return duration.count();
}

template <typename S, typename D>
Int Functions<S,D>
::p3_main(
  const P3Runtime& runtime_options,
  const P3PrognosticState& prognostic_state,
  const P3DiagnosticInputs& diagnostic_inputs,
  const P3DiagnosticOutputs& diagnostic_outputs,
  const P3Infrastructure& infrastructure,
  const P3HistoryOnly& history_only,
  const P3LookupTables& lookup_tables,
#ifdef SCREAM_P3_SMALL_KERNELS
  const P3Temporaries& temporaries,
#endif
  const WorkspaceManager& workspace_mgr,
  Int nj,
  Int nk)
{
#ifdef SCREAM_P3_SMALL_KERNELS
  return p3_main_internal_disp(runtime_options,
                               prognostic_state,
                               diagnostic_inputs,
                               diagnostic_outputs,
                               infrastructure,
                               history_only,
                               lookup_tables,
                               temporaries,
                               workspace_mgr,
                               nj, nk);
#else
  return p3_main_internal(runtime_options,
                          prognostic_state,
                          diagnostic_inputs,
                          diagnostic_outputs,
                          infrastructure,
                          history_only,
                          lookup_tables,
                          workspace_mgr,
                          nj, nk);
#endif
}
} // namespace p3
} // namespace scream

#endif // P3_MAIN_IMPL_HPP
