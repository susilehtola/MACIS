#include <mpi.h>
#include <iostream>
#include "cmz_ed/utils.h++"
#include "dbwy/csr_hamiltonian.hpp"
#include "dbwy/davidson.hpp"
#include "cmz_ed/slaterdet.h++"
#include "cmz_ed/integrals.h++"
#include "cmz_ed/hamil.h++"
#include "cmz_ed/lanczos.h++"
#include "cmz_ed/rdms.h++"

#include <bitset>

template <size_t N>
std::vector<std::bitset<N>> build_combs( uint64_t nbits, uint64_t nset ) {

  std::vector<bool> v(nbits, false);
  std::fill_n( v.begin(), nset, true );
  std::vector<std::bitset<N>> store;

  do {

    std::bitset<N> temp = 0ul;
    std::bitset<N> one  = 1ul;
    for( uint64_t i = 0; i < nbits; ++i )
    if( v[i] ) {
      temp = temp | (one << i);
    }
    store.emplace_back(temp);

  } while ( std::prev_permutation( v.begin(), v.end() ) );

  return store;

}

template <size_t N>
std::vector<std::bitset<N>> build_hilbert_space(
  size_t norbs, size_t nalpha, size_t nbeta
) {

  // Get all alpha and beta combs
  auto alpha_dets = build_combs<N>( norbs, nalpha );
  auto beta_dets  = build_combs<N>( norbs, nbeta  );

  std::vector<std::bitset<N>> states;
  states.reserve( alpha_dets.size() * beta_dets.size() );
  for( auto alpha_det : alpha_dets )
  for( auto beta_det  : beta_dets  ) {
    std::bitset<N> state = alpha_det | (beta_det << (N/2));
    states.emplace_back( state );
  }

  return states;
 
}

int main( int argc, char* argv[] ) {

  MPI_Init(NULL,NULL);
  int world_rank; MPI_Comm_rank(MPI_COMM_WORLD,&world_rank);
  int world_size; MPI_Comm_size(MPI_COMM_WORLD,&world_size);

  { // MPI Scope

  if( argc != 2 ) {
    std::cout << "Must Specify Input" << std::endl;
    return 1;
  }

  // Read Input
  std::string in_file = argv[1];
  cmz::ed::Input_t input;
  cmz::ed::ReadInput( in_file, input );

  // Get Parameters
  size_t norb = cmz::ed::getParam<int>( input, "norbs" );
  size_t nalpha  = cmz::ed::getParam<int>( input, "nups"  );
  size_t nbeta  = cmz::ed::getParam<int>( input, "ndos"  );
  size_t norb_eff = 
    cmz::ed::getParam<int>( input, "norbseff"  );
  std::string fcidump = 
    cmz::ed::getParam<std::string>( input, "fcidump_file" );

  bool do_fci = cmz::ed::getParam<bool>( input, "fci" );

  std::string wfn_file; 
  if(!do_fci) {
    try {
      wfn_file = cmz::ed::getParam<std::string>( input, "wfn_file" );
    } catch(...) {
      std::cout << "Must Specify Wfn File for non-FCI" << std::endl;
      throw;
    }
  }


#if 0
  // Read-In FCI file
  size_t norb2 = norb * norb;
  size_t norb3 = norb * norb2;
  size_t norb4 = norb * norb3;
  std::vector<double> V_pqrs(norb4), T_pq(norb2);
  double core_energy = 0.;
  {
    std::ifstream ifile(fcidump);
    std::string line;
    while( std::getline( ifile, line ) ) {
      if( line.find("ORB") != std::string::npos ) continue;
      if( line.find("SYM") != std::string::npos ) continue;
      if( line.find("END") != std::string::npos ) continue;
      // Split the line
      std::stringstream ss(line);

      // Read the line
      uint64_t p,q,r,s;
      double matel;
      ss >> p >> q >> r >> s >> matel;
      if(!ss) throw "Bad Read";

      if( p > norb or q > norb or r > norb or s > norb )
        throw "Bad Orbital Index";


      // Zero Matrix Element
      if( std::abs(matel) < 
        std::numeric_limits<double>::epsilon() ) continue;

      // Read Core Energy
      if( p == 0 and q == 0 and r == 0 and s == 0 ) {
        core_energy = matel;
      } else if( r == 0 and s == 0 ) {
        //std::cout << "T " << p << ", " << q << std::endl;
        T_pq[(p-1) + (q-1)*norb] = matel;
      } else {
        //std::cout << "V " << p << ", " << q << ", " << r << ", " << s << std::endl;
        V_pqrs[(p-1) + (q-1)*norb + (r-1)*norb2 + (s-1)*norb3] = matel;
      }
    }
  }
#endif

  cmz::ed::intgrls::integrals ints(norb, fcidump);

  constexpr size_t nbits = 128;
  std::vector<std::bitset<nbits>> dets;
  if( do_fci ) {
    // do FCI
    dets = build_hilbert_space<nbits>( norb_eff, nalpha, nbeta );
  } else {
    // Read in ASCI wfn
    std::ifstream ifile( wfn_file );
    std::string line;
    std::getline(ifile, line);
    while( std::getline(ifile, line) ) {
      std::stringstream ss(line);
      double c; size_t i,j;
      ss >> c >> i >> j;
      std::bitset<nbits> alpha = i;
      std::bitset<nbits> beta  = j;
      dets.emplace_back( alpha | (beta << (nbits/2)) );
    }
  }

  if(world_rank == 0)
    std::cout << "NDETS = " << dets.size() << std::endl;

  dbwy::DoubleLoopHamiltonianGenerator<nbits> 
    ham_gen( norb, ints.u.data(), ints.t.data() );
  auto H = dbwy::make_dist_csr_hamiltonian<int32_t>(MPI_COMM_WORLD,
    dets.begin(), dets.end(), ham_gen, 1e-12 );

  std::vector<double> X_local( H.local_row_extent() );
  auto E0 = p_davidson( 100, H, 1e-8, X_local.data() );
  if(world_rank == 0)
    std::cout << "\nE0 = " <<  E0 + ints.core_energy << std::endl;

  // Gather Eigenvector
  std::vector<double> X( world_rank ? 0 : dets.size() );
  {
  std::vector<int> recvcounts(world_size), recvdisp(world_size);
  recvdisp[0] = 0;
  recvcounts[0] = H.row_extent(0);
  for( auto i = 1; i < world_size; ++i ) {
    auto rcnt = H.row_extent(i);
    recvcounts[i] = rcnt;
    recvdisp[i] = recvdisp[i-1] + recvcounts[i-1];
  }
  MPI_Gatherv( X_local.data(), X_local.size(), MPI_DOUBLE,
    X.data(), recvcounts.data(), recvdisp.data(), MPI_DOUBLE,
    0, MPI_COMM_WORLD );
  }

  double print_tol = 1e-2;
  if( world_rank == 0 ) {
    std::cout << "Psi0 Eigenvector (tol = " << print_tol << "):" << std::endl;
    for( auto i = 0; i < dets.size(); ++i ) 
    if( std::abs(X[i]) >= print_tol ) {
      std::cout << "  " << std::setw(5) << std::left << i << " " << std::setw(20) << std::right << X[i] << std::endl;
    }
  }


  } // MPI Scope
  MPI_Finalize();

}
