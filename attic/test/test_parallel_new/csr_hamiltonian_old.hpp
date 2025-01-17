// This creates a block of the hamiltonian
// H( bra_begin:bra_end, ket_begin:ket_end )
// Currently double loops, but should delegate if the bra/ket coencide in the future
template <typename index_t>
sparsexx::csr_matrix<double,index_t> make_csr_hamiltonian_block(
  std::vector<slater_det>::iterator bra_begin,
  std::vector<slater_det>::iterator bra_end,
  std::vector<slater_det>::iterator ket_begin,
  std::vector<slater_det>::iterator ket_end,
  const FermionHamil&  Hop,
  const intgrls::integrals& ints,
  const double H_thresh
) {

  const size_t nbra_dets = std::distance( bra_begin, bra_end );
  const size_t nket_dets = std::distance( ket_begin, ket_end );

  const size_t  norb = bra_begin->GetNorbs();
  auto full_mask = [](unsigned nbits) {
    return (1ul << nbits) - 1ul;
  };

  const uint64_t alpha_mask = full_mask(norb);
  const uint64_t beta_mask  = alpha_mask << norb;

  std::vector<uint64_t> bra_states( nbra_dets );
  std::vector<uint64_t> ket_states( nket_dets );

  // Extract states for superior memory access
  auto get_state = [](const auto& s){ return s.GetState(); };
  std::transform( bra_begin, bra_end, bra_states.begin(), get_state );
  std::transform( ket_begin, ket_end, ket_states.begin(), get_state );

  // Preallocate colind / nzval indirection
  std::vector< std::vector<index_t> > colind_by_row( nbra_dets );
  std::vector< std::vector<double> >  nzval_by_row ( nbra_dets );

  const size_t res_count = 10000;
  for( auto& v : colind_by_row ) v.reserve( res_count );
  for( auto& v : nzval_by_row )  v.reserve( res_count );

  std::array<size_t,6> cases = {0,0,0,0,0,0};

  const double* V_pqrs = ints.u.data();
  const double* T_pq   = ints.t.data();
  const size_t  norb2 = norb  * norb;
  const size_t  norb3 = norb2 * norb;
  const size_t  norb4 = norb3 * norb;

  std::vector<double> G_pqrs(V_pqrs, V_pqrs + norb4);
  for( auto i = 0; i < norb; ++i )
  for( auto j = 0; j < norb; ++j )
  for( auto k = 0; k < norb; ++k )
  for( auto l = 0; l < norb; ++l ) {
    G_pqrs[i + j*norb + k*norb2 + l*norb3] -=
      V_pqrs[i + l*norb + k*norb2 + j*norb3];
  }
  //std::cout << "G4 = " << (G_pqrs.size()*8.)/1024/1024/1024 << " GB " << std::endl;

  std::vector<double> G_red(norb3), V_red(norb3);
  for( auto j = 0; j < norb; ++j ) 
  for( auto i = 0; i < norb; ++i )
  for( auto k = 0; k < norb; ++k ) {
    G_red[k + i*norb + j*norb2 ] = G_pqrs[k*(norb+1) + i*norb2 + j*norb3];
    V_red[k + i*norb + j*norb2 ] = V_pqrs[k*(norb+1) + i*norb2 + j*norb3];
  }
  //std::cout << "G3 = " << (G_red.size()*8.)/1024/1024/1024 << " GB " << std::endl;
  //std::cout << "V3 = " << (V_red.size()*8.)/1024/1024/1024 << " VB " << std::endl;

  std::vector<double> G2_red(norb2), V2_red(norb2);
  for( auto j = 0; j < norb; ++j ) 
  for( auto i = 0; i < norb; ++i ) {
    G2_red[i + j*norb] = 0.5 * G_pqrs[i*(norb+1) + j*(norb2+norb3)];
    V2_red[i + j*norb] = V_pqrs[i*(norb+1) + j*(norb2+norb3)];
  }

  //std::cout << "G2 = " << (G2_red.size()*8.)/1024/1024/1024 << " GB " << std::endl;
  //std::cout << "V2 = " << (V2_red.size()*8.)/1024/1024/1024 << " VB " << std::endl;


  auto first_occ_flipped = [=]( uint64_t state, uint64_t ex ) {
    return (ffsll( state & ex ) - 1ul ) % norb;
  };

  auto first_occ_flipped_up = [=]( uint64_t state, uint64_t ex ) {
    return (ffsll( state & ex & alpha_mask ) - 1ul) % norb;
  };
  auto first_occ_flipped_do = [=]( uint64_t state, uint64_t ex ) {
    return (ffsll( state & ex & beta_mask ) - 1ul) % norb;
  };

  auto single_ex_up_sign = []( uint64_t state, uint64_t p, uint64_t q ) {
    uint64_t mask = 0x0;
    if( p > q ) {
      mask = state & (((1ul << p) - 1ul) ^ ((1ul << (q+1)) - 1ul));
    } else {
      mask = state & (((1ul << q) - 1ul) ^ ((1ul << (p+1)) - 1ul));
    }
    return (std::popcount(mask) % 2) ? -1. : 1.;
  };

  auto single_ex_do_sign = [&]( uint64_t state, uint64_t p, uint64_t q ) {
    return single_ex_up_sign(state, p+norb, q+norb);
  };


  // Construct adjacencey
  #pragma omp parallel
  {
  std::vector<uint32_t> bra_occ_up, bra_occ_do;
  bra_occ_up.reserve(norb);
  bra_occ_do.reserve(norb);
  #pragma omp for 
  for( index_t i = 0; i < nbra_dets; ++i ) {

    //std::cout << i << std::endl;
    uint64_t bra = bra_states[i];
    if(bra == 0) continue;

    // Determine which orbitals are occupied in the bra det
    bra_occ_up.clear();
    bra_occ_do.clear();
    uint64_t up_mask = 1, do_mask = (1ul << norb);
    for( index_t p = 0; p < norb; ++p ) {
      if( bra & up_mask ) bra_occ_up.emplace_back(p);
      if( bra & do_mask ) bra_occ_do.emplace_back(p);
      up_mask = up_mask << 1;
      do_mask = do_mask << 1;
    }

  for( index_t j = 0; j < nket_dets; ++j ) { 
    uint64_t ket = ket_states[j];
    if(ket == 0) continue;

    uint64_t ex_total = bra ^ ket;
    uint64_t ex_count = std::popcount( ex_total );
    if( ex_count <= 4 ) {

      const uint64_t ex_up_count = std::popcount(ex_total & alpha_mask); 
      const uint64_t ex_do_count = ex_count - ex_up_count;

      double h_el = 0;
      if( ex_up_count == 4 and ex_do_count == 0 ) {

        // Get first single excition (up)
        const uint64_t o1 = first_occ_flipped( ket, ex_total );
        const uint64_t v1 = first_occ_flipped( bra, ex_total );
        auto sign = single_ex_up_sign( ket, v1, o1 );

        // Apply first single excitation (up)
        ket ^= (1ul << v1) ^ (1ul << o1);
        ex_total = bra ^ ket;

        // Get second single excitation (down)
        const uint64_t o2 = first_occ_flipped( ket, ex_total );
        const uint64_t v2 = first_occ_flipped( bra, ex_total );
        sign *= single_ex_up_sign( ket, v2, o2 );
        
        h_el = sign * G_pqrs[v1 + o1*norb + v2*norb2 + o2*norb3];
         
        #if 0
        auto tmp = Hop.GetHmatel( *(bra_begin+i), *(ket_begin+j) );
        if( std::abs(tmp - h_el) > 1e-11 ) std::cout << "Wrong Case 0" << std::endl;
        #endif

        cases[0]++;
      } else if (ex_up_count == 0 and ex_do_count == 4 ) {

        // Get first single excition (down)
        const uint64_t o1 = first_occ_flipped( ket, ex_total );
        const uint64_t v1 = first_occ_flipped( bra, ex_total );
        auto sign = single_ex_do_sign( ket, v1, o1 );

        // Apply first single excitation (down)
        ket ^= (1ul << (v1+norb)) ^ (1ul << (o1+norb));
        ex_total = bra ^ ket;

        // Get second single excitation (down)
        const uint64_t o2 = first_occ_flipped( ket, ex_total );
        const uint64_t v2 = first_occ_flipped( bra, ex_total );
        sign *= single_ex_do_sign( ket, v2, o2 );
        
        h_el = sign * G_pqrs[v1 + o1*norb + v2*norb2 + o2*norb3];

        #if 0
        auto tmp = Hop.GetHmatel( *(bra_begin+i), *(ket_begin+j) );
        if( std::abs(tmp - h_el) > 1e-11 ) std::cout << "Wrong Case 1" << std::endl;
        #endif
        cases[1]++;
      } else if (ex_up_count == 2 and ex_do_count == 2 ) {

        // Get first single excition (up)
        const uint64_t o1 = first_occ_flipped_up( ket, ex_total );
        const uint64_t v1 = first_occ_flipped_up( bra, ex_total );
        auto sign = single_ex_up_sign( ket, v1, o1 );

        // Apply first single excitation (up)
        ket ^= (1ul << v1) ^ (1ul << o1);
        ex_total = bra ^ ket;

        // Get second single excitation (down)
        const uint64_t o2 = first_occ_flipped_do( ket, ex_total );
        const uint64_t v2 = first_occ_flipped_do( bra, ex_total );
        sign *= single_ex_do_sign( ket, v2, o2 );
        
        h_el = sign * V_pqrs[v1 + o1*norb + v2*norb2 + o2*norb3]; 

        #if 0
        auto tmp = Hop.GetHmatel( *(bra_begin+i), *(ket_begin+j) );
        if( std::abs(tmp - h_el) > 1e-11 ) 
          std::cout << "Wrong Case 2 " << std::abs(tmp - h_el) << std::endl;
        #endif

        cases[2]++;
      } else if( ex_up_count == 2 and ex_do_count == 0 ) {

        // Get first single excition (up)
        const uint64_t o1 = first_occ_flipped( bra, ex_total );
        const uint64_t v1 = first_occ_flipped( ket, ex_total );
        auto sign = single_ex_up_sign( bra, v1, o1 );

        h_el = T_pq[v1 + o1*norb];
        for( auto p : bra_occ_up ) {
          h_el += G_red[ p + v1*norb + o1*norb2 ];
        }

        for( auto p : bra_occ_do ) {
          h_el += V_red[ p + v1*norb + o1*norb2 ];
        }  

        h_el *= sign;

        #if 0
        auto tmp = Hop.GetHmatel( *(bra_begin+i), *(ket_begin+j) );
        if( std::abs(tmp - h_el) > 1e-11 ) 
          std::cout << "Wrong Case 3 " << std::abs(tmp - h_el) << std::endl;
        #endif

        cases[3]++;
      } else if (ex_up_count == 0 and ex_do_count == 2 ) {

        // Get first single excition (down)
        #if 0
        const uint64_t o1 = first_occ_flipped( ket, ex_total );
        const uint64_t v1 = first_occ_flipped( bra, ex_total );
        auto sign = single_ex_do_sign( ket, v1, o1 );

        slater_det bra_sd = *(bra_begin + i);
        slater_det ket_sd = *(ket_begin + j);
        std::vector<uint64_t> ket_occ_up = ket_sd.GetOccOrbsUp();
        std::vector<uint64_t> ket_occ_do = ket_sd.GetOccOrbsDo();
        const auto& occ_up_use = ket_occ_up;
        const auto& occ_do_use = ket_occ_do;
        #else
        const uint64_t o1 = first_occ_flipped( bra, ex_total );
        const uint64_t v1 = first_occ_flipped( ket, ex_total );
        auto sign = single_ex_do_sign( bra, v1, o1 );

        const auto& occ_up_use = bra_occ_up;
        const auto& occ_do_use = bra_occ_do;
        #endif


        h_el = T_pq[v1 + o1*norb];
        for( auto p : occ_do_use ) {
          h_el += G_red[ p + v1*norb + o1*norb2 ];
        }

        for( auto p : occ_up_use ) {
          h_el += V_red[ p + v1*norb + o1*norb2 ];
        }  

        h_el *= sign;

        #if 0
        auto tmp = Hop.GetHmatel( *(bra_begin+i), *(ket_begin+j) );
        if( std::abs(tmp - h_el) > 1e-11 ) 
          std::cout << "Wrong Case 4 " << std::abs(tmp - h_el) << ", " <<
                       tmp << ", " << h_el << ", " << o1 << ", " << v1 << std::endl;
        #endif

        cases[4]++;
      } else {

        h_el = 0.;
        for( auto p : bra_occ_up ) h_el += T_pq[p*(norb+1)];
        for( auto p : bra_occ_do ) h_el += T_pq[p*(norb+1)];

        for( auto p : bra_occ_up )
        for( auto q : bra_occ_up ) {
          h_el += G2_red[p + q*norb];
        }

        for( auto p : bra_occ_do )
        for( auto q : bra_occ_do ) {
          h_el += G2_red[p + q*norb];
        }

        for( auto p : bra_occ_up )
        for( auto q : bra_occ_do ) {
          h_el += V2_red[p + q*norb];
        }

        #if 0
        auto tmp = Hop.GetHmatel( *(bra_begin+i), *(ket_begin+j) );
        if( std::abs(tmp - h_el) > 1e-11 ) 
          std::cout << "Wrong Case 5 " << std::abs(tmp - h_el) << std::endl;
        #endif

        cases[5]++;
      }

      if( std::abs(h_el) > H_thresh ) {
        colind_by_row[i].emplace_back( j );
        nzval_by_row [i].emplace_back( h_el );
      }
    }
  } // Ket Loop
  } // Bra Loop

  } // Open MP context

  //for( int i = 0; i < 6; ++i )
  //  std::cout << "CASE[" << i << "] = " << cases[i] << std::endl;

  // Compute row counts
  std::vector< size_t > row_counts( nbra_dets );
  std::transform( colind_by_row.begin(), colind_by_row.end(), row_counts.begin(),
    [](const auto& v){ return v.size(); } );

  // Compute NNZ 
  const size_t nnz = std::accumulate( row_counts.begin(), row_counts.end(), 0ul );

  sparsexx::csr_matrix<double, index_t> H( nbra_dets, nket_dets, nnz, 0 );
  auto& rowptr = H.rowptr();
  auto& colind = H.colind();
  auto& nzval  = H.nzval();

  // Compute rowptr
  std::exclusive_scan( row_counts.begin(), row_counts.end(), rowptr.begin(), 0 );
  rowptr[nbra_dets] = rowptr[nbra_dets - 1] + row_counts.back();

  // Linearize colind/nzval
  auto linearize_vov = []( const auto& vov, auto& lin ) {
    auto it = lin.begin();
    for( const auto& v : vov ) {
      it = std::copy( v.begin(), v.end(), it );
    }
  };

  linearize_vov( colind_by_row, colind );
  linearize_vov( nzval_by_row,  nzval  );

  return H;

}

