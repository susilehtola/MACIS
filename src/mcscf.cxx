#include <asci/util/mcscf.hpp>
#include <asci/util/orbital_gradient.hpp>
#include <asci/util/fock_matrices.hpp>
#include <asci/util/transform.hpp>
#include <asci/util/selected_ci_diag.hpp>
#include <asci/hamiltonian_generator/double_loop.hpp>
#include <asci/davidson.hpp>
#include <asci/fcidump.hpp>
#include <Eigen/Core>

#include "orbital_rotation_utilities.hpp"
#include "orbital_hessian.hpp"
#include "orbital_steps.hpp"
#include "diis.hpp"

namespace asci {

template <typename HamGen>
double compute_casci_rdms(MCSCFSettings settings, NumOrbital norb, 
  size_t nalpha, size_t nbeta, double* T, double* V, double* ORDM, 
  double* TRDM, std::vector<double>& C, MPI_Comm comm) {

  constexpr auto nbits = HamGen::nbits;

  int rank; MPI_Comm_rank(comm, &rank);

  // Hamiltonian Matrix Element Generator
  size_t no = norb.get();
  HamGen ham_gen( 
    matrix_span<double>(T,no,no),
    rank4_span<double>(V,no,no,no,no) 
  );
  
  // Compute Lowest Energy Eigenvalue (ED)
  auto dets = asci::generate_hilbert_space<nbits>(norb.get(), nalpha, nbeta);
  double E0 = asci::selected_ci_diag( dets.begin(), dets.end(), ham_gen,
    settings.ci_matel_tol, settings.ci_max_subspace, settings.ci_res_tol, C, 
    comm, true);

  // Compute RDMs
  ham_gen.form_rdms(dets.begin(), dets.end(), dets.begin(), dets.end(),
    C.data(), matrix_span<double>(ORDM,no,no), 
    rank4_span<double>(TRDM,no,no,no,no));

  return E0;
}


template <typename HamGen>
double casscf_bfgs_impl(MCSCFSettings settings, NumElectron nalpha, 
  NumElectron nbeta, NumOrbital norb, NumInactive ninact, NumActive nact, 
  NumVirtual nvirt, double E_core, double* T, size_t LDT, double* V, size_t LDV, 
  double* A1RDM, size_t LDD1, double* A2RDM, size_t LDD2, MPI_Comm comm) {


  /******************************************************************
   *  Top of CASSCF Routine - Setup and print header info to logger *
   ******************************************************************/

  auto logger = spdlog::get("casscf");
  if(!logger) logger = spdlog::stdout_color_mt("casscf");

  logger->info("[MCSCF Settings]:");
  logger->info(
    "  {:13} = {:4}, {:13} = {:3}, {:13} = {:3}",
    "NACTIVE_ALPHA", nalpha.get(),
    "NACTIVE_BETA" , nbeta.get(),
    "NORB_TOTAL",    norb.get()
  );
  logger->info(
    "  {:13} = {:4}, {:13} = {:3}, {:13} = {:3}",
    "NINACTIVE", ninact.get(),
    "NACTIVE",    nact.get(),
    "NVIRTUAL",  nvirt.get()
  );
  logger->info(
    "  {:13} = {:4}, {:13} = {:3}, {:13} = {:3}",
    "ENABLE_DIIS", settings.enable_diis,
    "DIIS_START",  settings.diis_start_iter,
    "DIIS_NKEEP",  settings.diis_nkeep
  );
  logger->info( "  {:13} = {:.6f}", "E_CORE", E_core);
  logger->info( 
    "  {:13} = {:.6e}, {:13} = {:.6e}",
    "MAX_ORB_STEP", settings.max_orbital_step,
    "ORBGRAD_TOL", settings.orb_grad_tol_mcscf
    //"BFGS_TOL",    settings.orb_grad_tol_bfgs,
    //"BFGS_MAX_ITER", settings.max_bfgs_iter
   );
  logger->info("  {:13} = {:.6e}, {:13} = {:.6e}, {:13} = {:3}",
    "CI_RES_TOL",  settings.ci_res_tol,
    "CI_MATEL_TOL", settings.ci_matel_tol, 
    "CI_MAX_SUB",   settings.ci_max_subspace
  );


  // MCSCF Iteration format string
  const std::string fmt_string = 
    "iter = {:4} E(CI) = {:.10f}, dE = {:18.10e}, |orb_rms| = {:18.10e}";



  /*********************************************************
   *  Calculate persistant derived dimensions to be reused *
   *  throughout this routine                              *
   *********************************************************/

  const size_t no = norb.get(), ni = ninact.get(), na = nact.get(), 
               nv = nvirt.get();

  const size_t no2 = no  * no;
  const size_t no4 = no2 * no2;
  const size_t na2 = na  * na;
  const size_t na4 = na2 * na2;

  const size_t orb_rot_sz = nv*(ni + na) + na*ni;
  const double rms_factor = std::sqrt(orb_rot_sz);
  logger->info("  {:13} = {}","ORB_ROT_SZ", orb_rot_sz);






  /********************************************************
   *               Allocate persistant data               *
   ********************************************************/ 

  // Energies
  double E_inactive, E0;

  // Convergence data
  double grad_nrm;
  bool converged = false;

  // Storage for active space Hamitonian
  std::vector<double> T_active(na2), V_active(na4);

  // CI vector - will be resized on first CI call
  std::vector<double> X_CI; 

  // Orbital Gradient and Generalized Fock Matrix
  std::vector<double> F(no2), OG(orb_rot_sz), F_inactive(no2), 
    F_active(no2), Q(na * no);

  // Storage for transformed integrals
  std::vector<double> transT(T, T+no2), transV(V, V+no4);

  // Storage for total transformation
  std::vector<double> U_total(no2, 0.0), K_total(no2, 0.0);

  // DIIS Object
  DIIS<std::vector<double>> diis(settings.diis_nkeep);






  /**************************************************************
   *    Precompute Active Space Hamiltonian given input data    *
   *                                                            *
   *     This will be used to compute initial energies and      *
   *      gradients to decide whether to proceed with the       *
   *                   MCSCF optimization.                      *
   **************************************************************/

  // Compute Active Space Hamiltonian and Inactive Fock Matrix
  active_hamiltonian(norb, nact, ninact, T, LDT, V, LDV, 
    F_inactive.data(), no, T_active.data(), na, 
    V_active.data(), na);

  // Compute Inactive Energy
  E_inactive = inactive_energy(ninact, T, LDT, F_inactive.data(), no);
  E_inactive += E_core;





  /**************************************************************
   *     Either compute or read initial RDMs from input         *
   *                                                            *
   * If the trace of the input 1RDM is != to the total number   *
   * of active electrons, RDMs will be computed, otherwise the  *
   *      input RDMs will be taken as an initial guess.         *
   **************************************************************/

  // Compute the trace of the input A1RDM
  double iAtr = 0.0;
  for(size_t i = 0; i < na; ++i) iAtr += A1RDM[i*(LDD1+1)];
  bool comp_rdms = std::abs(iAtr - nalpha.get() - nbeta.get()) > 1e-6; 

  if(comp_rdms) {
    // Compute active RDMs
    logger->info("Computing Initial RDMs");
    std::fill_n(A1RDM, na2, 0.0);
    std::fill_n(A2RDM, na4, 0.0);
    compute_casci_rdms<HamGen>(settings, NumOrbital(na), nalpha.get(), 
      nbeta.get(), T_active.data(), V_active.data(), A1RDM, A2RDM, X_CI,
      comm) + E_inactive;
  } else {
    logger->info("Using Passed RDMs");
  }


  /***************************************************************
   * Compute initial energy and gradient from computed (or read) * 
   * RDMs                                                        *
   ***************************************************************/

  // Compute Energy from RDMs
  double E_1RDM = blas::dot(na2, A1RDM, 1, T_active.data(), 1);
  double E_2RDM = blas::dot(na4, A2RDM, 1, V_active.data(), 1);

  E0 =  E_1RDM + E_2RDM + E_inactive; 
  logger->info("{:8} = {:20.12f}","E(1RDM)",E_1RDM);
  logger->info("{:8} = {:20.12f}","E(2RDM)",E_2RDM);
  logger->info("{:8} = {:20.12f}","E(CI)",E0);


  // Compute initial Fock and gradient
  active_fock_matrix(norb, ninact, nact, V, LDV, A1RDM, LDD1, 
    F_active.data(), no );
  aux_q_matrix(nact, norb, ninact, V, LDV, A2RDM, LDD2, 
    Q.data(), na);
  generalized_fock_matrix( norb, ninact, nact, F_inactive.data(), no, 
    F_active.data(), no, A1RDM, LDD1, Q.data(), na, F.data(), no);
  fock_to_linear_orb_grad(ninact, nact, nvirt, F.data(), no,
    OG.data());





  /**************************************************************
   *      Compute initial Gradient norm and decide whether      *
   *           input data is sufficiently converged             *
   **************************************************************/

  grad_nrm = blas::nrm2(OG.size(), OG.data(), 1);
  converged = grad_nrm < settings.orb_grad_tol_mcscf;
  logger->info(fmt_string, 0, E0, 0.0, grad_nrm/rms_factor);





  /**************************************************************
   *                     MCSCF Iterations                       *
   **************************************************************/

  for(size_t iter = 0; iter < settings.max_macro_iter; ++iter) {

     // Check for convergence signal
     if(converged) break;

     // Save old data 
     const double E0_old = E0;
     std::vector<double> K_total_sav(K_total); 

    /************************************************************
     *                  Compute Orbital Step                    *
     ************************************************************/

     std::vector<double> K_step(no2);

     // Compute the step in linear storage
     std::vector<double> K_step_linear(orb_rot_sz);
     precond_cg_orbital_step(norb, ninact, nact, nvirt, F_inactive.data(),
       no, F_active.data(), no, F.data(), no, A1RDM, LDD1, 
       OG.data(), K_step_linear.data());

     // Compute norms / max
     auto step_nrm  = blas::nrm2(orb_rot_sz, K_step_linear.data(), 1);
     auto step_amax = std::abs(
       K_step_linear[ blas::iamax(orb_rot_sz, K_step_linear.data(), 1) ] 
     );
     logger->debug("{:12}step_nrm = {:.4e}, step_amax = {:.4e}", "", 
       step_nrm, step_amax);

     // Scale step if necessacary 
     const auto max_step = settings.max_orbital_step;
     if( step_amax > max_step ) { 
       logger->info("  * decresing step from {:.2f} to {:.2f}", 
         step_amax, max_step);
       blas::scal(orb_rot_sz, max_step / step_amax, K_step_linear.data(), 1);
     }

     // Expand info full matrix
     linear_orb_rot_to_matrix(ninact, nact, nvirt, K_step_linear.data(),
       K_step.data(), no);

     // Increment total step
     blas::axpy(no2, 1.0, K_step.data(), 1, K_total.data(), 1);

     // DIIS Extrapolation
     if(settings.enable_diis and iter >= settings.diis_start_iter) {
       diis.add_vector(K_total, OG);
       if(iter >= (settings.diis_start_iter+2)) {
         K_total = diis.extrapolate();
       }
     }



    /************************************************************
     *   Compute orbital rotation matrix corresponding to the   * 
     *                 total (accumulated) step                 *
     ************************************************************/
     if(!iter) {

       // If its the first iteration U_total = EXP[-K_total]
       compute_orbital_rotation(norb, 1.0, K_total.data(), no, 
         U_total.data(), no );

     } else {

       // Compute the rotation matrix for the *actual* step taken, 
       // accounting for possible extrapolation
       // 
       // U_step = EXP[-(K_total - K_total_old)]
       std::vector<double> U_step(no2);
       blas::axpy(no2, -1.0, K_total.data(), 1, K_total_sav.data(), 1);
       blas::scal(no2, -1.0, K_total_sav.data(), 1);
       compute_orbital_rotation(norb, 1.0, K_total_sav.data(), no, 
         U_step.data(), no );

       // U_total = U_total * U_step
       std::vector<double> tmp(no2);
       blas::gemm(blas::Layout::ColMajor,
         blas::Op::NoTrans, blas::Op::NoTrans,
         no, no, no, 
         1.0, U_total.data(), no, U_step.data(), no,
         0.0, tmp.data(), no
       );

       U_total = std::move(tmp);
     }


    /************************************************************
     *          Transform Hamiltonian into new MO basis         * 
     ************************************************************/
     two_index_transform(no, no, T, LDT, U_total.data(), no, 
       transT.data(), no);
     four_index_transform(no, no, 0, V, LDV, U_total.data(), no, 
       transV.data(), no);

     
    /************************************************************
     *      Compute Active Space Hamiltonian and associated     *
     *                    scalar quantities                     *
     ************************************************************/

     // Compute Active Space Hamiltonian + inactive Fock
     active_hamiltonian(norb, nact, ninact, transT.data(), no, transV.data(), no, 
      F_inactive.data(), no, T_active.data(), na, V_active.data(), na);

     // Compute Inactive Energy
     E_inactive = inactive_energy(ninact, transT.data(), no, 
       F_inactive.data(), no) + E_core;



    /************************************************************
     *       Compute new Active Space RDMs and GS energy        *
     ************************************************************/

     std::fill_n( A1RDM, na2, 0.0);
     std::fill_n( A2RDM, na4, 0.0);
     E0 = compute_casci_rdms<HamGen>(settings, NumOrbital(na), 
       nalpha.get(), nbeta.get(), T_active.data(), V_active.data(), A1RDM, 
       A2RDM, X_CI, comm) + E_inactive;

    /************************************************************
     *               Compute new Orbital Gradient               *
     ************************************************************/

    std::fill(F.begin(), F.end(), 0.0);

    // Update active fock + Q
    active_fock_matrix(norb, ninact, nact, transV.data(), no, A1RDM, LDD1, 
      F_active.data(), no );
    aux_q_matrix(nact, norb, ninact, transV.data(), no, A2RDM, LDD2, 
      Q.data(), na);

    // Compute Fock
    generalized_fock_matrix( norb, ninact, nact, F_inactive.data(), no, 
      F_active.data(), no, A1RDM, LDD1, Q.data(), na, F.data(), no);
    fock_to_linear_orb_grad(ninact, nact, nvirt, F.data(), no,
      OG.data());

    // Gradient Norm
    grad_nrm = blas::nrm2(OG.size(), OG.data(), 1);
    logger->info(fmt_string, iter+1, E0, E0 - E0_old, grad_nrm/rms_factor);

    converged = grad_nrm/rms_factor < settings.orb_grad_tol_mcscf;
  }

  if(converged) logger->info("CASSCF Converged");
  return E0;
}



double casscf_bfgs(MCSCFSettings settings, NumElectron nalpha, NumElectron nbeta, 
  NumOrbital norb, NumInactive ninact, NumActive nact, NumVirtual nvirt, 
  double E_core, double* T, size_t LDT, double* V, size_t LDV, 
  double* A1RDM, size_t LDD1, double* A2RDM, size_t LDD2, MPI_Comm comm) { 

  using generator_t = DoubleLoopHamiltonianGenerator<64>;
  return casscf_bfgs_impl<generator_t>(settings, nalpha, nbeta, norb, ninact, nact, 
    nvirt, E_core, T, LDT, V, LDV, A1RDM, LDD1, A2RDM, LDD2, comm);

}

}
