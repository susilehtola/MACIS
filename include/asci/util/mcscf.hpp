#pragma once
#include <asci/types.hpp>
#include <mpi.h>


namespace asci {

struct MCSCFSettings {

  size_t max_macro_iter     = 100;
  double max_orbital_step   = 0.5;
  double orb_grad_tol_mcscf = 5e-6;

  bool   enable_diis        = true;
  size_t diis_start_iter    = 3;
  size_t diis_nkeep         = 10;

  //size_t max_bfgs_iter      = 100;
  //double orb_grad_tol_bfgs  = 5e-7;

  double ci_res_tol         = 1e-8;
  size_t ci_max_subspace    = 20;
  double ci_matel_tol       = std::numeric_limits<double>::epsilon();
};

#if 0
void optimize_orbitals(MCSCFSettings settings, NumOrbital norb, 
  NumInactive ninact, NumActive nact, NumVirtual nvirt, double E_core, 
  const double* T, size_t LDT, const double* V, size_t LDV, 
  const double* A1RDM, size_t LDD1, const double* A2RDM, size_t LDD2, 
  double *K, size_t LDK);
#endif

double casscf_bfgs(MCSCFSettings settings, NumElectron nalpha, NumElectron nbeta, 
  NumOrbital norb, NumInactive ninact, NumActive nact, NumVirtual nvirt, 
  double E_core, double* T, size_t LDT, double* V, size_t LDV, 
  double* A1RDM, size_t LDD1, double* A2RDM, size_t LDD2, MPI_Comm comm); 

}
