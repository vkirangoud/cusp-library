/*
 *  Copyright 2008-2009 NVIDIA Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <cusp/blas.h>
#include <cusp/multiply.h>
#include <cusp/monitor.h>
#include <cusp/transpose.h>
#include <cusp/graph/maximal_independent_set.h>
#include <cusp/precond/diagonal.h>
#include <cusp/precond/aggregate.h>
#include <cusp/precond/strength.h>
#include <cusp/krylov/arnoldi.h>

#include <cusp/detail/format_utils.h>
#include <cusp/detail/spectral_radius.h>

#include <thrust/extrema.h>
#include <thrust/transform.h>
#include <thrust/gather.h>
#include <thrust/reduce.h>

namespace cusp
{
namespace precond
{
namespace detail
{


template <typename MatrixType>
struct Dinv_A : public cusp::linear_operator<typename MatrixType::value_type, typename MatrixType::memory_space>
{
    const MatrixType& A;
    const cusp::precond::diagonal<typename MatrixType::value_type, typename MatrixType::memory_space> Dinv;

    Dinv_A(const MatrixType& A)
        : A(A), Dinv(A),
          cusp::linear_operator<typename MatrixType::value_type, typename MatrixType::memory_space>(A.num_rows, A.num_cols, A.num_entries + A.num_rows)
          {}

    template <typename Array1, typename Array2>
    void operator()(const Array1& x, Array2& y) const
    {
        cusp::multiply(A,x,y);
        cusp::multiply(Dinv,y,y);
    }
};

template <typename MatrixType>
double estimate_rho_Dinv_A(const MatrixType& A)
{
    typedef typename MatrixType::value_type   ValueType;
    typedef typename MatrixType::memory_space MemorySpace;

    Dinv_A<MatrixType> Dinv_A(A);

    return cusp::detail::ritz_spectral_radius(Dinv_A);
}


template <typename T>
struct square : thrust::unary_function<T,T>
{
    __host__ __device__
    T operator()(const T& x) { return x * x; }
};

template <typename T>
struct sqrt_functor : thrust::unary_function<T,T>
{
    __host__ __device__
    T operator()(const T& x) { return sqrt(x); }
};

template <typename Array1,
          typename Array2,
          typename IndexType, typename ValueType, typename MemorySpace,
          typename Array3>
void fit_candidates(const Array1& aggregates,
                    const Array2& B,
                          cusp::coo_matrix<IndexType,ValueType,MemorySpace>& Q,
                          Array3& R)
{
  CUSP_PROFILE_SCOPED();
  // TODO handle case w/ unaggregated nodes (marked w/ -1)
  IndexType num_aggregates = *thrust::max_element(aggregates.begin(), aggregates.end()) + 1;

  Q.resize(aggregates.size(), num_aggregates, aggregates.size());
  R.resize(num_aggregates);

  // gather values into Q
  thrust::sequence(Q.row_indices.begin(), Q.row_indices.end());
  thrust::copy(aggregates.begin(), aggregates.end(), Q.column_indices.begin());
  thrust::copy(B.begin(), B.end(), Q.values.begin());
                        
  // compute norm over each aggregate
  {
    // compute Qt
    cusp::coo_matrix<IndexType,ValueType,MemorySpace> Qt;  cusp::transpose(Q, Qt);

    // compute sum of squares for each column of Q (rows of Qt)
    cusp::array1d<IndexType, MemorySpace> temp(num_aggregates);
    thrust::reduce_by_key(Qt.row_indices.begin(), Qt.row_indices.end(),
                          thrust::make_transform_iterator(Qt.values.begin(), square<ValueType>()),
                          temp.begin(),
                          R.begin());

    // compute square root of each column sum
    thrust::transform(R.begin(), R.end(), R.begin(), sqrt_functor<ValueType>());
  }

  // rescale columns of Q
  thrust::transform(Q.values.begin(), Q.values.end(),
                    thrust::make_permutation_iterator(R.begin(), Q.column_indices.begin()),
                    Q.values.begin(),
                    thrust::divides<ValueType>());
}


//   Smoothed (final) prolongator defined by P = (I - omega/rho(K) K) * T
//   where K = diag(S)^-1 * S and rho(K) is an approximation to the 
//   spectral radius of K.
template <typename IndexType, typename ValueType, typename MemorySpace>
void smooth_prolongator(const cusp::coo_matrix<IndexType,ValueType,MemorySpace>& S,
                        const cusp::coo_matrix<IndexType,ValueType,MemorySpace>& T,
                              cusp::coo_matrix<IndexType,ValueType,MemorySpace>& P,
                        const ValueType omega = 4.0/3.0,
                        const ValueType rho_Dinv_S = 0.0)
{
  CUSP_PROFILE_SCOPED();

  // TODO handle case with unaggregated nodes
  assert(T.num_entries == T.num_rows);

  const ValueType lambda = omega / (rho_Dinv_S == 0.0 ? estimate_rho_Dinv_A(S) : rho_Dinv_S);

  // temp <- lambda * S(i,j) * T(j,k)
  cusp::coo_matrix<IndexType,ValueType,MemorySpace> temp(S.num_rows, T.num_cols, S.num_entries + T.num_entries);
  thrust::copy(S.row_indices.begin(), S.row_indices.end(), temp.row_indices.begin());
#if THRUST_VERSION >= 100300
  thrust::gather(S.column_indices.begin(), S.column_indices.end(), T.column_indices.begin(), temp.column_indices.begin());
#else
  // TODO remove this when Thrust v1.2.x is unsupported
  thrust::next::gather(S.column_indices.begin(), S.column_indices.end(), T.column_indices.begin(), temp.column_indices.begin());
#endif 
  thrust::transform(S.values.begin(), S.values.end(),
                    thrust::make_permutation_iterator(T.values.begin(), S.column_indices.begin()),
                    temp.values.begin(),
                    thrust::multiplies<ValueType>());
  thrust::transform(temp.values.begin(), temp.values.begin() + S.num_entries,
                    thrust::constant_iterator<ValueType>(-lambda),
                    temp.values.begin(),
                    thrust::multiplies<ValueType>());
  // temp <- D^-1
  {
    cusp::array1d<ValueType, MemorySpace> D(S.num_rows);
    cusp::detail::extract_diagonal(S, D);
    thrust::transform(temp.values.begin(), temp.values.begin() + S.num_entries,
                      thrust::make_permutation_iterator(D.begin(), S.row_indices.begin()),
                      temp.values.begin(),
                      thrust::divides<ValueType>());
  }

  // temp <- temp + T
  thrust::copy(T.row_indices.begin(),    T.row_indices.end(),    temp.row_indices.begin()    + S.num_entries);
  thrust::copy(T.column_indices.begin(), T.column_indices.end(), temp.column_indices.begin() + S.num_entries);
  thrust::copy(T.values.begin(),         T.values.end(),         temp.values.begin()         + S.num_entries);

  // sort by (I,J)
  cusp::detail::sort_by_row_and_column(temp.row_indices, temp.column_indices, temp.values);

  // compute unique number of nonzeros in the output
  // throws a warning at compile (warning: expression has no effect)
  IndexType NNZ = thrust::inner_product(thrust::make_zip_iterator(thrust::make_tuple(temp.row_indices.begin(), temp.column_indices.begin())),
                                        thrust::make_zip_iterator(thrust::make_tuple(temp.row_indices.end (),  temp.column_indices.end()))   - 1,
                                        thrust::make_zip_iterator(thrust::make_tuple(temp.row_indices.begin(), temp.column_indices.begin())) + 1,
                                        IndexType(0),
                                        thrust::plus<IndexType>(),
                                        thrust::not_equal_to< thrust::tuple<IndexType,IndexType> >()) + 1;

  // allocate space for output
  P.resize(temp.num_rows, temp.num_cols, NNZ);

  // sum values with the same (i,j)
  thrust::reduce_by_key(thrust::make_zip_iterator(thrust::make_tuple(temp.row_indices.begin(), temp.column_indices.begin())),
                        thrust::make_zip_iterator(thrust::make_tuple(temp.row_indices.end(),   temp.column_indices.end())),
                        temp.values.begin(),
                        thrust::make_zip_iterator(thrust::make_tuple(P.row_indices.begin(), P.column_indices.begin())),
                        P.values.begin(),
                        thrust::equal_to< thrust::tuple<IndexType,IndexType> >(),
                        thrust::plus<ValueType>());
}

template <typename IndexType, typename ValueType, typename MemorySpace, typename CsrView>
void setup_view(cusp::coo_matrix<IndexType,ValueType,MemorySpace> & M,
		CsrView & M_view, 
		cusp::array1d<IndexType,MemorySpace> & row_offsets)
{
	CUSP_PROFILE_SCOPED();

	row_offsets.resize(M.num_rows+1);
	cusp::detail::indices_to_offsets(M.row_indices, row_offsets);
	M_view = CsrView(M.num_rows, M.num_cols, M.num_entries,
	cusp::make_array1d_view(row_offsets),
	cusp::make_array1d_view(M.column_indices),
	cusp::make_array1d_view(M.values));
}

} // end namespace detail


template <typename IndexType, typename ValueType, typename MemorySpace>
smoothed_aggregation<IndexType,ValueType,MemorySpace>::smoothed_aggregation(const cusp::coo_matrix<IndexType,ValueType,MemorySpace>& A, const ValueType theta)
    : theta(theta)
{
  CUSP_PROFILE_SCOPED();

  levels.reserve(20); // avoid reallocations which force matrix copies

  levels.push_back(typename smoothed_aggregation<IndexType,ValueType,MemorySpace>::level());
  levels.back().A = A; // copy
  levels.back().B.resize(A.num_rows, 1.0f);

  while (levels.back().A.num_rows > 100)
    extend_hierarchy();

  // TODO make lu_solver accept sparse input
  cusp::array2d<ValueType,cusp::host_memory> coarse_dense(levels.back().A);
  LU = cusp::detail::lu_solver<ValueType, cusp::host_memory>(coarse_dense);
}

template <typename IndexType, typename ValueType, typename MemorySpace>
void smoothed_aggregation<IndexType,ValueType,MemorySpace>::extend_hierarchy(void)
{
  CUSP_PROFILE_SCOPED();

  const cusp::coo_matrix<IndexType,ValueType,MemorySpace>& A = levels.back().A;
  const cusp::array1d<ValueType,MemorySpace>&              B = levels.back().B;

  // compute stength of connection matrix
  cusp::coo_matrix<IndexType,ValueType,MemorySpace> C;
  detail::symmetric_strength_of_connection(A, C, theta);

  detail::setup_view( levels.back().A, levels.back().A_view, levels.back().A_row_offsets );
  // compute spectral radius of diag(C)^-1 * C
  ValueType rho_DinvA = detail::estimate_rho_Dinv_A(levels.back().A_view);

  // compute aggregates
  cusp::array1d<IndexType,MemorySpace> aggregates(C.num_rows,0);
  detail::standard_aggregation(C, aggregates);

  // compute tenative prolongator and coarse nullspace vector
  cusp::coo_matrix<IndexType,ValueType,MemorySpace> T;
  cusp::array1d<ValueType,MemorySpace>              B_coarse;
  detail::fit_candidates(aggregates, B, T, B_coarse);
  
  // compute prolongation operator
  cusp::coo_matrix<IndexType,ValueType,MemorySpace> P;
  detail::smooth_prolongator(A, T, P, ValueType(4.0/3.0), rho_DinvA);  // TODO if C != A then compute rho_Dinv_C

  // compute restriction operator (transpose of prolongator)
  cusp::coo_matrix<IndexType,ValueType,MemorySpace> R;
  cusp::transpose(P,R);

  // construct Galerkin product R*A*P
  cusp::coo_matrix<IndexType,ValueType,MemorySpace> RAP;
  {
    // TODO test speed of R * (A * P) vs. (R * A) * P
    cusp::coo_matrix<IndexType,ValueType,MemorySpace> AP;
    cusp::multiply(A, P, AP);
    cusp::multiply(R, AP, RAP);
  }

  #ifndef USE_POLY_SMOOTHER
  //  4/3 * 1/rho is a good default, where rho is the spectral radius of D^-1(A)
  ValueType omega = ValueType(4.0/3.0) / rho_DinvA;
  levels.back().smoother = cusp::relaxation::jacobi<ValueType, MemorySpace>(A, omega);
  #else
  cusp::array1d<ValueType,cusp::host_memory> coeff;
  ValueType rho = cusp::detail::ritz_spectral_radius(A);
  cusp::relaxation::detail::chebyshev_polynomial_coefficients(rho,coeff);
  levels.back().smoother = cusp::relaxation::polynomial<ValueType, MemorySpace>(A, coeff);
  #endif

  levels.back().aggregates.swap(aggregates);
  levels.back().R.swap(R);
  levels.back().P.swap(P);
  levels.back().residual.resize(levels.back().A.num_rows);

  //std::cout << "omega " << omega << std::endl;

  detail::setup_view( levels.back().R, levels.back().R_view, levels.back().R_row_offsets );
  detail::setup_view( levels.back().P, levels.back().P_view, levels.back().P_row_offsets );

  levels.push_back(level());
  levels.back().A.swap(RAP);
  levels.back().B.swap(B_coarse);
  levels.back().x.resize(levels.back().A.num_rows);
  levels.back().b.resize(levels.back().A.num_rows);

}

    
template <typename IndexType, typename ValueType, typename MemorySpace>
template <typename Array1, typename Array2>
void smoothed_aggregation<IndexType,ValueType,MemorySpace>::operator()(const Array1& b, Array2& x)
{
  CUSP_PROFILE_SCOPED();

  // perform 1 V-cycle
  _solve(b, x, 0);
}

template <typename IndexType, typename ValueType, typename MemorySpace>
void smoothed_aggregation<IndexType,ValueType,MemorySpace>::solve(const cusp::array1d<ValueType,MemorySpace>& b,
                                                                        cusp::array1d<ValueType,MemorySpace>& x)
{
  CUSP_PROFILE_SCOPED();

  cusp::default_monitor<ValueType> monitor(b);

  solve(b, x, monitor);
}

template <typename IndexType, typename ValueType, typename MemorySpace>
template <typename Monitor>
void smoothed_aggregation<IndexType,ValueType,MemorySpace>::solve(const cusp::array1d<ValueType,MemorySpace>& b,
                                                                        cusp::array1d<ValueType,MemorySpace>& x,
                                                                        Monitor& monitor )
{
  CUSP_PROFILE_SCOPED();

  // create a csr_matrix_view
  typedef typename cusp::array1d<IndexType,MemorySpace>::iterator       IndexIterator;
  typedef typename cusp::array1d<ValueType,MemorySpace>::iterator       ValueIterator;
  typedef typename cusp::array1d_view<IndexIterator>                    IndexView;
  typedef typename cusp::array1d_view<ValueIterator>                    ValueView;
  typedef typename cusp::csr_matrix_view<IndexView,IndexView,ValueView> CsrView;
  // TODO check sizes
  const CsrView & A = levels[0].A_view;
 
  // use simple iteration
  cusp::array1d<ValueType,MemorySpace> update(A.num_rows);
  cusp::array1d<ValueType,MemorySpace> residual(A.num_rows);

  // compute initial residual
  cusp::multiply(A,x,residual);
  cusp::blas::axpby(b, residual, residual, ValueType(1.0), ValueType(-1.0));

  while(!monitor.finished(residual))
  {   
      _solve(residual, update, 0); 

      // x += M * r
      cusp::blas::axpy(update, x, ValueType(1.0));

      // update residual
      cusp::multiply(A,x,residual);
      cusp::blas::axpby(b, residual, residual, ValueType(1.0), ValueType(-1.0));
      ++monitor;
  }   
}

template <typename IndexType, typename ValueType, typename MemorySpace>
void smoothed_aggregation<IndexType,ValueType,MemorySpace>
::_solve(const cusp::array1d<ValueType,MemorySpace>& b,
               cusp::array1d<ValueType,MemorySpace>& x,
         const int i)
{
  CUSP_PROFILE_SCOPED();

  typedef typename cusp::array1d<IndexType,MemorySpace>::iterator       IndexIterator;
  typedef typename cusp::array1d<ValueType,MemorySpace>::iterator       ValueIterator;
  typedef typename cusp::array1d_view<IndexIterator>                    IndexView;
  typedef typename cusp::array1d_view<ValueIterator>                    ValueView;
  typedef typename cusp::csr_matrix_view<IndexView,IndexView,ValueView> CsrView;

  if (i + 1 == levels.size())
  {
    // coarse grid solve
    // TODO streamline
    cusp::array1d<ValueType,cusp::host_memory> temp_b(b);
    cusp::array1d<ValueType,cusp::host_memory> temp_x(x.size());
    LU(temp_b, temp_x);
    x = temp_x;
  }
  else
  {
    const CsrView & R = levels[i].R_view;
    const CsrView & A = levels[i].A_view;
    const CsrView & P = levels[i].P_view;

    cusp::array1d<ValueType,MemorySpace>& residual = levels[i].residual;
    cusp::array1d<ValueType,MemorySpace>& coarse_b = levels[i + 1].b;
    cusp::array1d<ValueType,MemorySpace>& coarse_x = levels[i + 1].x;

    // presmooth
    levels[i].smoother.presmooth(A,b,x);

    // compute residual <- b - A*x
    cusp::multiply(A, x, residual);
    cusp::blas::axpby(b, residual, residual, ValueType(1.0), ValueType(-1.0));

    // restrict to coarse grid
    cusp::multiply(R, residual, coarse_b);

    // compute coarse grid solution
    _solve(coarse_b, coarse_x, i + 1);

    // apply coarse grid correction 
    cusp::multiply(P, coarse_x, residual);
    cusp::blas::axpy(residual, x, ValueType(1.0));

    // postsmooth
    levels[i].smoother.postsmooth(A,b,x);
  }
}

template <typename IndexType, typename ValueType, typename MemorySpace>
void smoothed_aggregation<IndexType,ValueType,MemorySpace>
::print( void )
{
	IndexType num_levels = levels.size();

	std::cout << "\tNumber of Levels:\t" << num_levels << std::endl;
	std::cout << "\tOperator Complexity:\t" << operator_complexity() << std::endl;
	std::cout << "\tGrid Complexity:\t" << grid_complexity() << std::endl;
	std::cout << "\tlevel\tunknowns\tnonzeros:\t" << std::endl;

	IndexType nnz = 0;

	for( IndexType index = 0; index < levels.size(); index++ )
		nnz += levels[index].A.num_entries;

	for( IndexType index = 0; index < levels.size(); index++ )
  {
		double percent = (double)levels[index].A.num_entries / nnz;
		std::cout << "\t" << index << "\t" << levels[index].A.num_cols << "\t\t" \
              << levels[index].A.num_entries << " \t[" << 100*percent << "%]" \
              << std::endl;
	}
} 

template <typename IndexType, typename ValueType, typename MemorySpace>
double smoothed_aggregation<IndexType,ValueType,MemorySpace>
::operator_complexity( void )
{
	IndexType nnz = 0;

	for( IndexType index = 0; index < levels.size(); index++ )
		nnz += levels[index].A.num_entries;

	return (double) nnz / levels[0].A.num_entries;
} 

template <typename IndexType, typename ValueType, typename MemorySpace>
double smoothed_aggregation<IndexType,ValueType,MemorySpace>
::grid_complexity( void )
{
	IndexType unknowns = 0;
	for( IndexType index = 0; index < levels.size(); index++ )
		unknowns += levels[index].A.num_rows;

	return (double) unknowns / levels[0].A.num_rows;
} 

} // end namespace precond
} // end namespace cusp

