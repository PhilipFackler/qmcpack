//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2019 QMCPACK developers.
//
// File developed by: Jeongnim Kim, jeongnim.kim@intel.com, Intel Corp.
//                    Ye Luo, yeluo@anl.gov, Argonne National Laboratory
//
// File created by: Jeongnim Kim, jeongnim.kim@intel.com, Intel Corp.
//////////////////////////////////////////////////////////////////////////////////////


#include <complex>
#include "Concurrency/OpenMP.h"
#include "SplineC2CT.h"
#include "spline2/MultiBsplineEval.hpp"
#include "QMCWaveFunctions/BsplineFactory/contraction_helper.hpp"
#include "CPU/math.hpp"

namespace qmcplusplus
{
template<class T>
SplineC2CT<T>::SplineC2CT(const SplineC2CT& in) = default;

template<class T>
inline void SplineC2CT<T>::set_spline(SingleSplineType* spline_r,
                                      SingleSplineType* spline_i,
                                      int twist,
                                      int ispline,
                                      int level)
{
  SplineInst->copy_spline(spline_r, 2 * ispline);
  SplineInst->copy_spline(spline_i, 2 * ispline + 1);
}

template<class T>
bool SplineC2CT<T>::read_splines(hdf_archive& h5f)
{
  std::ostringstream o;
  o << "spline_" << this->MyIndex;
  einspline_engine<SplineType> bigtable(SplineInst->getSplinePtr());
  return h5f.readEntry(bigtable, o.str().c_str()); //"spline_0");
}

template<class T>
bool SplineC2CT<T>::write_splines(hdf_archive& h5f)
{
  std::ostringstream o;
  o << "spline_" << this->MyIndex;
  einspline_engine<SplineType> bigtable(SplineInst->getSplinePtr());
  return h5f.writeEntry(bigtable, o.str().c_str()); //"spline_0");
}

template<class T>
void SplineC2CT<T>::storeParamsBeforeRotation()
{
  const auto spline_ptr     = SplineInst->getSplinePtr();
  const auto coefs_tot_size = spline_ptr->coefs_size;
  coef_copy_                = std::make_shared<std::vector<RealType>>(coefs_tot_size);

  std::copy_n(spline_ptr->coefs, coefs_tot_size, coef_copy_->begin());
}

/*
  ~~ Notes for rotation ~~
  spl_coefs      = Raw pointer to spline coefficients
  basis_set_size = Number of spline coefs per orbital
  OrbitalSetSize = Number of orbitals (excluding padding)

  spl_coefs has a complicated layout depending on dimensionality of splines.
  Luckily, for our purposes, we can think of spl_coefs as pointing to a
  matrix of size BasisSetSize x (OrbitalSetSize + padding), with the spline
  index adjacent in memory. The orbital index is SIMD aligned and therefore
  may include padding.

  As a result, due to SIMD alignment, Nsplines may be larger than the
  actual number of splined orbitals. This means that in practice rot_mat
  may be smaller than the number of 'columns' in the coefs array!

      SplineR2R spl_coef layout:
             ^         | sp1 | ... | spN | pad |
             |         |=====|=====|=====|=====|
             |         | c11 | ... | c1N | 0   |
      basis_set_size   | c21 | ... | c2N | 0   |
             |         | ... | ... | ... | 0   |
             |         | cM1 | ... | cMN | 0   |
             v         |=====|=====|=====|=====|
                       <------ Nsplines ------>

      SplineC2C spl_coef layout:
             ^         | sp1_r | sp1_i |  ...  | spN_r | spN_i |  pad  |
             |         |=======|=======|=======|=======|=======|=======|
             |         | c11_r | c11_i |  ...  | c1N_r | c1N_i |   0   |
      basis_set_size   | c21_r | c21_i |  ...  | c2N_r | c2N_i |   0   |
             |         |  ...  |  ...  |  ...  |  ...  |  ...  |  ...  |
             |         | cM1_r | cM1_i |  ...  | cMN_r | cMN_i |   0   |
             v         |=======|=======|=======|=======|=======|=======|
                       <------------------ Nsplines ------------------>

  NB: For splines (typically) BasisSetSize >> OrbitalSetSize, so the spl_coefs
  "matrix" is very tall and skinny.
*/
template<class T>
void SplineC2CT<T>::applyRotation(const ValueMatrix& rot_mat, bool use_stored_copy)
{
  // SplineInst is a MultiBspline. See src/spline2/MultiBspline.hpp
  const auto spline_ptr = SplineInst->getSplinePtr();
  assert(spline_ptr != nullptr);
  const auto spl_coefs      = spline_ptr->coefs;
  const auto Nsplines       = spline_ptr->num_splines; // May include padding
  const auto coefs_tot_size = spline_ptr->coefs_size;
  const auto basis_set_size = coefs_tot_size / Nsplines;
  assert(this->OrbitalSetSize == rot_mat.rows());
  assert(this->OrbitalSetSize == rot_mat.cols());

  if (!use_stored_copy)
  {
    assert(coef_copy_ != nullptr);
    std::copy_n(spl_coefs, coefs_tot_size, coef_copy_->begin());
  }

  for (int i = 0; i < basis_set_size; i++)
    for (int j = 0; j < this->OrbitalSetSize; j++)
    {
      // cur_elem points to the real componend of the coefficient.
      // Imag component is adjacent in memory.
      const auto cur_elem = Nsplines * i + 2 * j;
      RealType newval_r{0.};
      RealType newval_i{0.};
      for (auto k = 0; k < this->OrbitalSetSize; k++)
      {
        const auto index = Nsplines * i + 2 * k;
        RealType zr      = (*coef_copy_)[index];
        RealType zi      = (*coef_copy_)[index + 1];
        RealType wr      = rot_mat[k][j].real();
        RealType wi      = rot_mat[k][j].imag();
        newval_r += zr * wr - zi * wi;
        newval_i += zr * wi + zi * wr;
      }
      spl_coefs[cur_elem]     = newval_r;
      spl_coefs[cur_elem + 1] = newval_i;
    }
}

template<class T>
inline void SplineC2CT<T>::assign_v(const PointType& r,
                                    const vContainer_type& myV,
                                    ValueVector& psi,
                                    int first,
                                    int last) const
{
  const auto kPointsSize = this->kPoints.size();
  // protect last
  last = last > kPointsSize ? kPointsSize : last;

  const RealType x = r[0], y = r[1], z = r[2];
  const RealType* restrict kx = myKcart.data(0);
  const RealType* restrict ky = myKcart.data(1);
  const RealType* restrict kz = myKcart.data(2);
#pragma omp simd
  for (size_t j = first; j < last; ++j)
  {
    RealType s, c;
    const RealType val_r = myV[2 * j];
    const RealType val_i = myV[2 * j + 1];
    qmcplusplus::sincos(-(x * kx[j] + y * ky[j] + z * kz[j]), &s, &c);
    psi[j + this->first_spo] = ComplexT(val_r * c - val_i * s, val_i * c + val_r * s);
  }
}

template<class T>
void SplineC2CT<T>::evaluateValue(const ParticleSetT<T>& P, const int iat, ValueVector& psi)
{
  const PointType& r = P.activeR(iat);
  PointType ru(PrimLattice.toUnit_floor(r));

#pragma omp parallel
  {
    int first, last;
    // Factor of 2 because psi is complex and the spline storage and evaluation uses a real type
    FairDivideAligned(2 * psi.size(), getAlignment<T>(), omp_get_num_threads(), omp_get_thread_num(), first, last);

    spline2::evaluate3d(SplineInst->getSplinePtr(), ru, myV, first, last);
    assign_v(r, myV, psi, first / 2, last / 2);
  }
}

template<class T>
void SplineC2CT<T>::evaluateDetRatios(const VirtualParticleSetT<T>& VP,
                                      ValueVector& psi,
                                      const ValueVector& psiinv,
                                      std::vector<ValueType>& ratios)
{
  const bool need_resize = ratios_private.rows() < VP.getTotalNum();

#pragma omp parallel
  {
    int tid = omp_get_thread_num();
    // initialize thread private ratios
    if (need_resize)
    {
      if (tid == 0) // just like #pragma omp master, but one fewer call to the runtime
        ratios_private.resize(VP.getTotalNum(), omp_get_num_threads());
#pragma omp barrier
    }
    int first, last;
    // Factor of 2 because psi is complex and the spline storage and evaluation uses a real type
    FairDivideAligned(2 * psi.size(), getAlignment<T>(), omp_get_num_threads(), tid, first, last);
    const int first_cplx   = first / 2;
    const auto kPointsSize = this->kPoints.size();
    const int last_cplx    = kPointsSize < last / 2 ? kPointsSize : last / 2;

    for (int iat = 0; iat < VP.getTotalNum(); ++iat)
    {
      const PointType& r = VP.activeR(iat);
      PointType ru(PrimLattice.toUnit_floor(r));

      spline2::evaluate3d(SplineInst->getSplinePtr(), ru, myV, first, last);
      assign_v(r, myV, psi, first_cplx, last_cplx);
      ratios_private[iat][tid] = simd::dot(psi.data() + first_cplx, psiinv.data() + first_cplx, last_cplx - first_cplx);
    }
  }

  // do the reduction manually
  for (int iat = 0; iat < VP.getTotalNum(); ++iat)
  {
    ratios[iat] = ComplexT(0);
    for (int tid = 0; tid < ratios_private.cols(); tid++)
      ratios[iat] += ratios_private[iat][tid];
  }
}

/** assign_vgl
   */
template<class T>
inline void SplineC2CT<T>::assign_vgl(const PointType& r,
                                      ValueVector& psi,
                                      GradVector& dpsi,
                                      ValueVector& d2psi,
                                      int first,
                                      int last) const
{
  // protect last
  const auto kPointsSize = this->kPoints.size();
  last                   = last > kPointsSize ? kPointsSize : last;

  constexpr RealType zero(0);
  constexpr RealType two(2);
  const RealType g00 = PrimLattice.G(0), g01 = PrimLattice.G(1), g02 = PrimLattice.G(2), g10 = PrimLattice.G(3),
                 g11 = PrimLattice.G(4), g12 = PrimLattice.G(5), g20 = PrimLattice.G(6), g21 = PrimLattice.G(7),
                 g22 = PrimLattice.G(8);
  const RealType x = r[0], y = r[1], z = r[2];
  const RealType symGG[6] = {GGt[0], GGt[1] + GGt[3], GGt[2] + GGt[6], GGt[4], GGt[5] + GGt[7], GGt[8]};

  const RealType* restrict k0 = myKcart.data(0);
  const RealType* restrict k1 = myKcart.data(1);
  const RealType* restrict k2 = myKcart.data(2);

  const RealType* restrict g0  = myG.data(0);
  const RealType* restrict g1  = myG.data(1);
  const RealType* restrict g2  = myG.data(2);
  const RealType* restrict h00 = myH.data(0);
  const RealType* restrict h01 = myH.data(1);
  const RealType* restrict h02 = myH.data(2);
  const RealType* restrict h11 = myH.data(3);
  const RealType* restrict h12 = myH.data(4);
  const RealType* restrict h22 = myH.data(5);

#pragma omp simd
  for (size_t j = first; j < last; ++j)
  {
    const size_t jr = j << 1;
    const size_t ji = jr + 1;

    const RealType kX    = k0[j];
    const RealType kY    = k1[j];
    const RealType kZ    = k2[j];
    const RealType val_r = myV[jr];
    const RealType val_i = myV[ji];

    //phase
    RealType s, c;
    qmcplusplus::sincos(-(x * kX + y * kY + z * kZ), &s, &c);

    //dot(PrimLattice.G,myG[j])
    const RealType dX_r = g00 * g0[jr] + g01 * g1[jr] + g02 * g2[jr];
    const RealType dY_r = g10 * g0[jr] + g11 * g1[jr] + g12 * g2[jr];
    const RealType dZ_r = g20 * g0[jr] + g21 * g1[jr] + g22 * g2[jr];

    const RealType dX_i = g00 * g0[ji] + g01 * g1[ji] + g02 * g2[ji];
    const RealType dY_i = g10 * g0[ji] + g11 * g1[ji] + g12 * g2[ji];
    const RealType dZ_i = g20 * g0[ji] + g21 * g1[ji] + g22 * g2[ji];

    // \f$\nabla \psi_r + {\bf k}\psi_i\f$
    const RealType gX_r = dX_r + val_i * kX;
    const RealType gY_r = dY_r + val_i * kY;
    const RealType gZ_r = dZ_r + val_i * kZ;
    const RealType gX_i = dX_i - val_r * kX;
    const RealType gY_i = dY_i - val_r * kY;
    const RealType gZ_i = dZ_i - val_r * kZ;

    const RealType lcart_r = SymTrace(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], symGG);
    const RealType lcart_i = SymTrace(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], symGG);
    const RealType lap_r   = lcart_r + mKK[j] * val_r + two * (kX * dX_i + kY * dY_i + kZ * dZ_i);
    const RealType lap_i   = lcart_i + mKK[j] * val_i - two * (kX * dX_r + kY * dY_r + kZ * dZ_r);
    const size_t psiIndex  = j + this->first_spo;
    psi[psiIndex]          = ComplexT(c * val_r - s * val_i, c * val_i + s * val_r);
    dpsi[psiIndex][0]      = ComplexT(c * gX_r - s * gX_i, c * gX_i + s * gX_r);
    dpsi[psiIndex][1]      = ComplexT(c * gY_r - s * gY_i, c * gY_i + s * gY_r);
    dpsi[psiIndex][2]      = ComplexT(c * gZ_r - s * gZ_i, c * gZ_i + s * gZ_r);
    d2psi[psiIndex]        = ComplexT(c * lap_r - s * lap_i, c * lap_i + s * lap_r);
  }
}

/** assign_vgl_from_l can be used when myL is precomputed and myV,myG,myL in cartesian
   */
template<class T>
inline void SplineC2CT<T>::assign_vgl_from_l(const PointType& r, ValueVector& psi, GradVector& dpsi, ValueVector& d2psi)
{
  constexpr RealType two(2);
  const RealType x = r[0], y = r[1], z = r[2];

  const RealType* restrict k0 = myKcart.data(0);
  const RealType* restrict k1 = myKcart.data(1);
  const RealType* restrict k2 = myKcart.data(2);

  const RealType* restrict g0 = myG.data(0);
  const RealType* restrict g1 = myG.data(1);
  const RealType* restrict g2 = myG.data(2);

  const size_t N = this->last_spo - this->first_spo;
#pragma omp simd
  for (size_t j = 0; j < N; ++j)
  {
    const size_t jr = j << 1;
    const size_t ji = jr + 1;

    const RealType kX    = k0[j];
    const RealType kY    = k1[j];
    const RealType kZ    = k2[j];
    const RealType val_r = myV[jr];
    const RealType val_i = myV[ji];

    //phase
    RealType s, c;
    qmcplusplus::sincos(-(x * kX + y * kY + z * kZ), &s, &c);

    //dot(PrimLattice.G,myG[j])
    const RealType dX_r = g0[jr];
    const RealType dY_r = g1[jr];
    const RealType dZ_r = g2[jr];

    const RealType dX_i = g0[ji];
    const RealType dY_i = g1[ji];
    const RealType dZ_i = g2[ji];

    // \f$\nabla \psi_r + {\bf k}\psi_i\f$
    const RealType gX_r = dX_r + val_i * kX;
    const RealType gY_r = dY_r + val_i * kY;
    const RealType gZ_r = dZ_r + val_i * kZ;
    const RealType gX_i = dX_i - val_r * kX;
    const RealType gY_i = dY_i - val_r * kY;
    const RealType gZ_i = dZ_i - val_r * kZ;

    const RealType lap_r = myL[jr] + mKK[j] * val_r + two * (kX * dX_i + kY * dY_i + kZ * dZ_i);
    const RealType lap_i = myL[ji] + mKK[j] * val_i - two * (kX * dX_r + kY * dY_r + kZ * dZ_r);

    const size_t psiIndex = j + this->first_spo;
    psi[psiIndex]         = ComplexT(c * val_r - s * val_i, c * val_i + s * val_r);
    dpsi[psiIndex][0]     = ComplexT(c * gX_r - s * gX_i, c * gX_i + s * gX_r);
    dpsi[psiIndex][1]     = ComplexT(c * gY_r - s * gY_i, c * gY_i + s * gY_r);
    dpsi[psiIndex][2]     = ComplexT(c * gZ_r - s * gZ_i, c * gZ_i + s * gZ_r);
    d2psi[psiIndex]       = ComplexT(c * lap_r - s * lap_i, c * lap_i + s * lap_r);
  }
}

template<class T>
void SplineC2CT<T>::evaluateVGL(const ParticleSetT<T>& P,
                                const int iat,
                                ValueVector& psi,
                                GradVector& dpsi,
                                ValueVector& d2psi)
{
  const PointType& r = P.activeR(iat);
  PointType ru(PrimLattice.toUnit_floor(r));

#pragma omp parallel
  {
    int first, last;
    // Factor of 2 because psi is complex and the spline storage and evaluation uses a real type
    FairDivideAligned(2 * psi.size(), getAlignment<T>(), omp_get_num_threads(), omp_get_thread_num(), first, last);

    spline2::evaluate3d_vgh(SplineInst->getSplinePtr(), ru, myV, myG, myH, first, last);
    assign_vgl(r, psi, dpsi, d2psi, first / 2, last / 2);
  }
}

template<class T>
void SplineC2CT<T>::assign_vgh(const PointType& r,
                               ValueVector& psi,
                               GradVector& dpsi,
                               HessVector& grad_grad_psi,
                               int first,
                               int last) const
{
  // protect last
  const auto kPointsSize = this->kPoints.size();
  last                   = last > kPointsSize ? kPointsSize : last;

  const RealType g00 = PrimLattice.G(0), g01 = PrimLattice.G(1), g02 = PrimLattice.G(2), g10 = PrimLattice.G(3),
                 g11 = PrimLattice.G(4), g12 = PrimLattice.G(5), g20 = PrimLattice.G(6), g21 = PrimLattice.G(7),
                 g22 = PrimLattice.G(8);
  const RealType x = r[0], y = r[1], z = r[2];

  const RealType* restrict k0 = myKcart.data(0);
  const RealType* restrict k1 = myKcart.data(1);
  const RealType* restrict k2 = myKcart.data(2);

  const RealType* restrict g0  = myG.data(0);
  const RealType* restrict g1  = myG.data(1);
  const RealType* restrict g2  = myG.data(2);
  const RealType* restrict h00 = myH.data(0);
  const RealType* restrict h01 = myH.data(1);
  const RealType* restrict h02 = myH.data(2);
  const RealType* restrict h11 = myH.data(3);
  const RealType* restrict h12 = myH.data(4);
  const RealType* restrict h22 = myH.data(5);

#pragma omp simd
  for (size_t j = first; j < last; ++j)
  {
    int jr = j << 1;
    int ji = jr + 1;

    const RealType kX    = k0[j];
    const RealType kY    = k1[j];
    const RealType kZ    = k2[j];
    const RealType val_r = myV[jr];
    const RealType val_i = myV[ji];

    //phase
    RealType s, c;
    qmcplusplus::sincos(-(x * kX + y * kY + z * kZ), &s, &c);

    //dot(PrimLattice.G,myG[j])
    const RealType dX_r = g00 * g0[jr] + g01 * g1[jr] + g02 * g2[jr];
    const RealType dY_r = g10 * g0[jr] + g11 * g1[jr] + g12 * g2[jr];
    const RealType dZ_r = g20 * g0[jr] + g21 * g1[jr] + g22 * g2[jr];

    const RealType dX_i = g00 * g0[ji] + g01 * g1[ji] + g02 * g2[ji];
    const RealType dY_i = g10 * g0[ji] + g11 * g1[ji] + g12 * g2[ji];
    const RealType dZ_i = g20 * g0[ji] + g21 * g1[ji] + g22 * g2[ji];

    // \f$\nabla \psi_r + {\bf k}\psi_i\f$
    const RealType gX_r = dX_r + val_i * kX;
    const RealType gY_r = dY_r + val_i * kY;
    const RealType gZ_r = dZ_r + val_i * kZ;
    const RealType gX_i = dX_i - val_r * kX;
    const RealType gY_i = dY_i - val_r * kY;
    const RealType gZ_i = dZ_i - val_r * kZ;

    const size_t psiIndex = j + this->first_spo;
    psi[psiIndex]         = ComplexT(c * val_r - s * val_i, c * val_i + s * val_r);
    dpsi[psiIndex][0]     = ComplexT(c * gX_r - s * gX_i, c * gX_i + s * gX_r);
    dpsi[psiIndex][1]     = ComplexT(c * gY_r - s * gY_i, c * gY_i + s * gY_r);
    dpsi[psiIndex][2]     = ComplexT(c * gZ_r - s * gZ_i, c * gZ_i + s * gZ_r);

    const RealType h_xx_r =
        v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g00, g01, g02, g00, g01, g02) + kX * (gX_i + dX_i);
    const RealType h_xy_r =
        v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g00, g01, g02, g10, g11, g12) + kX * (gY_i + dY_i);
    const RealType h_xz_r =
        v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g00, g01, g02, g20, g21, g22) + kX * (gZ_i + dZ_i);
    const RealType h_yx_r =
        v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g10, g11, g12, g00, g01, g02) + kY * (gX_i + dX_i);
    const RealType h_yy_r =
        v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g10, g11, g12, g10, g11, g12) + kY * (gY_i + dY_i);
    const RealType h_yz_r =
        v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g10, g11, g12, g20, g21, g22) + kY * (gZ_i + dZ_i);
    const RealType h_zx_r =
        v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g20, g21, g22, g00, g01, g02) + kZ * (gX_i + dX_i);
    const RealType h_zy_r =
        v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g20, g21, g22, g10, g11, g12) + kZ * (gY_i + dY_i);
    const RealType h_zz_r =
        v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g20, g21, g22, g20, g21, g22) + kZ * (gZ_i + dZ_i);

    const RealType h_xx_i =
        v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g00, g01, g02, g00, g01, g02) - kX * (gX_r + dX_r);
    const RealType h_xy_i =
        v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g00, g01, g02, g10, g11, g12) - kX * (gY_r + dY_r);
    const RealType h_xz_i =
        v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g00, g01, g02, g20, g21, g22) - kX * (gZ_r + dZ_r);
    const RealType h_yx_i =
        v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g10, g11, g12, g00, g01, g02) - kY * (gX_r + dX_r);
    const RealType h_yy_i =
        v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g10, g11, g12, g10, g11, g12) - kY * (gY_r + dY_r);
    const RealType h_yz_i =
        v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g10, g11, g12, g20, g21, g22) - kY * (gZ_r + dZ_r);
    const RealType h_zx_i =
        v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g20, g21, g22, g00, g01, g02) - kZ * (gX_r + dX_r);
    const RealType h_zy_i =
        v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g20, g21, g22, g10, g11, g12) - kZ * (gY_r + dY_r);
    const RealType h_zz_i =
        v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g20, g21, g22, g20, g21, g22) - kZ * (gZ_r + dZ_r);

    grad_grad_psi[psiIndex][0] = ComplexT(c * h_xx_r - s * h_xx_i, c * h_xx_i + s * h_xx_r);
    grad_grad_psi[psiIndex][1] = ComplexT(c * h_xy_r - s * h_xy_i, c * h_xy_i + s * h_xy_r);
    grad_grad_psi[psiIndex][2] = ComplexT(c * h_xz_r - s * h_xz_i, c * h_xz_i + s * h_xz_r);
    grad_grad_psi[psiIndex][3] = ComplexT(c * h_yx_r - s * h_yx_i, c * h_yx_i + s * h_yx_r);
    grad_grad_psi[psiIndex][4] = ComplexT(c * h_yy_r - s * h_yy_i, c * h_yy_i + s * h_yy_r);
    grad_grad_psi[psiIndex][5] = ComplexT(c * h_yz_r - s * h_yz_i, c * h_yz_i + s * h_yz_r);
    grad_grad_psi[psiIndex][6] = ComplexT(c * h_zx_r - s * h_zx_i, c * h_zx_i + s * h_zx_r);
    grad_grad_psi[psiIndex][7] = ComplexT(c * h_zy_r - s * h_zy_i, c * h_zy_i + s * h_zy_r);
    grad_grad_psi[psiIndex][8] = ComplexT(c * h_zz_r - s * h_zz_i, c * h_zz_i + s * h_zz_r);
  }
}

template<class T>
void SplineC2CT<T>::evaluateVGH(const ParticleSetT<T>& P,
                                const int iat,
                                ValueVector& psi,
                                GradVector& dpsi,
                                HessVector& grad_grad_psi)
{
  const PointType& r = P.activeR(iat);
  PointType ru(PrimLattice.toUnit_floor(r));

#pragma omp parallel
  {
    int first, last;
    // Factor of 2 because psi is complex and the spline storage and evaluation uses a real type
    FairDivideAligned(2 * psi.size(), getAlignment<T>(), omp_get_num_threads(), omp_get_thread_num(), first, last);

    spline2::evaluate3d_vgh(SplineInst->getSplinePtr(), ru, myV, myG, myH, first, last);
    assign_vgh(r, psi, dpsi, grad_grad_psi, first / 2, last / 2);
  }
}

template<class T>
void SplineC2CT<T>::assign_vghgh(const PointType& r,
                                 ValueVector& psi,
                                 GradVector& dpsi,
                                 HessVector& grad_grad_psi,
                                 GGGVector& grad_grad_grad_psi,
                                 int first,
                                 int last) const
{
  // protect last
  const auto kPointsSize = this->kPoints.size();
  last                   = last < 0 ? kPointsSize : (last > kPointsSize ? kPointsSize : last);

  const RealType g00 = PrimLattice.G(0), g01 = PrimLattice.G(1), g02 = PrimLattice.G(2), g10 = PrimLattice.G(3),
                 g11 = PrimLattice.G(4), g12 = PrimLattice.G(5), g20 = PrimLattice.G(6), g21 = PrimLattice.G(7),
                 g22 = PrimLattice.G(8);
  const RealType x = r[0], y = r[1], z = r[2];

  const RealType* restrict k0 = myKcart.data(0);
  const RealType* restrict k1 = myKcart.data(1);
  const RealType* restrict k2 = myKcart.data(2);

  const RealType* restrict g0  = myG.data(0);
  const RealType* restrict g1  = myG.data(1);
  const RealType* restrict g2  = myG.data(2);
  const RealType* restrict h00 = myH.data(0);
  const RealType* restrict h01 = myH.data(1);
  const RealType* restrict h02 = myH.data(2);
  const RealType* restrict h11 = myH.data(3);
  const RealType* restrict h12 = myH.data(4);
  const RealType* restrict h22 = myH.data(5);

  const RealType* restrict gh000 = mygH.data(0);
  const RealType* restrict gh001 = mygH.data(1);
  const RealType* restrict gh002 = mygH.data(2);
  const RealType* restrict gh011 = mygH.data(3);
  const RealType* restrict gh012 = mygH.data(4);
  const RealType* restrict gh022 = mygH.data(5);
  const RealType* restrict gh111 = mygH.data(6);
  const RealType* restrict gh112 = mygH.data(7);
  const RealType* restrict gh122 = mygH.data(8);
  const RealType* restrict gh222 = mygH.data(9);

//SIMD doesn't work quite right yet.  Comment out until further debugging.
#pragma omp simd
  for (size_t j = first; j < last; ++j)
  {
    int jr = j << 1;
    int ji = jr + 1;

    const RealType kX    = k0[j];
    const RealType kY    = k1[j];
    const RealType kZ    = k2[j];
    const RealType val_r = myV[jr];
    const RealType val_i = myV[ji];

    //phase
    RealType s, c;
    qmcplusplus::sincos(-(x * kX + y * kY + z * kZ), &s, &c);

    //dot(PrimLattice.G,myG[j])
    const RealType dX_r = g00 * g0[jr] + g01 * g1[jr] + g02 * g2[jr];
    const RealType dY_r = g10 * g0[jr] + g11 * g1[jr] + g12 * g2[jr];
    const RealType dZ_r = g20 * g0[jr] + g21 * g1[jr] + g22 * g2[jr];

    const RealType dX_i = g00 * g0[ji] + g01 * g1[ji] + g02 * g2[ji];
    const RealType dY_i = g10 * g0[ji] + g11 * g1[ji] + g12 * g2[ji];
    const RealType dZ_i = g20 * g0[ji] + g21 * g1[ji] + g22 * g2[ji];

    // \f$\nabla \psi_r + {\bf k}\psi_i\f$
    const RealType gX_r = dX_r + val_i * kX;
    const RealType gY_r = dY_r + val_i * kY;
    const RealType gZ_r = dZ_r + val_i * kZ;
    const RealType gX_i = dX_i - val_r * kX;
    const RealType gY_i = dY_i - val_r * kY;
    const RealType gZ_i = dZ_i - val_r * kZ;

    const size_t psiIndex = j + this->first_spo;
    psi[psiIndex]         = ComplexT(c * val_r - s * val_i, c * val_i + s * val_r);
    dpsi[psiIndex][0]     = ComplexT(c * gX_r - s * gX_i, c * gX_i + s * gX_r);
    dpsi[psiIndex][1]     = ComplexT(c * gY_r - s * gY_i, c * gY_i + s * gY_r);
    dpsi[psiIndex][2]     = ComplexT(c * gZ_r - s * gZ_i, c * gZ_i + s * gZ_r);

    //intermediates for computation of hessian. \partial_i \partial_j phi in cartesian coordinates.
    const RealType f_xx_r = v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g00, g01, g02, g00, g01, g02);
    const RealType f_xy_r = v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g00, g01, g02, g10, g11, g12);
    const RealType f_xz_r = v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g00, g01, g02, g20, g21, g22);
    const RealType f_yy_r = v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g10, g11, g12, g10, g11, g12);
    const RealType f_yz_r = v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g10, g11, g12, g20, g21, g22);
    const RealType f_zz_r = v_m_v(h00[jr], h01[jr], h02[jr], h11[jr], h12[jr], h22[jr], g20, g21, g22, g20, g21, g22);

    const RealType f_xx_i = v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g00, g01, g02, g00, g01, g02);
    const RealType f_xy_i = v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g00, g01, g02, g10, g11, g12);
    const RealType f_xz_i = v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g00, g01, g02, g20, g21, g22);
    const RealType f_yy_i = v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g10, g11, g12, g10, g11, g12);
    const RealType f_yz_i = v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g10, g11, g12, g20, g21, g22);
    const RealType f_zz_i = v_m_v(h00[ji], h01[ji], h02[ji], h11[ji], h12[ji], h22[ji], g20, g21, g22, g20, g21, g22);

    const RealType h_xx_r = f_xx_r + 2 * kX * dX_i - kX * kX * val_r;
    const RealType h_xy_r = f_xy_r + (kX * dY_i + kY * dX_i) - kX * kY * val_r;
    const RealType h_xz_r = f_xz_r + (kX * dZ_i + kZ * dX_i) - kX * kZ * val_r;
    const RealType h_yy_r = f_yy_r + 2 * kY * dY_i - kY * kY * val_r;
    const RealType h_yz_r = f_yz_r + (kY * dZ_i + kZ * dY_i) - kY * kZ * val_r;
    const RealType h_zz_r = f_zz_r + 2 * kZ * dZ_i - kZ * kZ * val_r;

    const RealType h_xx_i = f_xx_i - 2 * kX * dX_r - kX * kX * val_i;
    const RealType h_xy_i = f_xy_i - (kX * dY_r + kY * dX_r) - kX * kY * val_i;
    const RealType h_xz_i = f_xz_i - (kX * dZ_r + kZ * dX_r) - kX * kZ * val_i;
    const RealType h_yy_i = f_yy_i - 2 * kY * dY_r - kY * kY * val_i;
    const RealType h_yz_i = f_yz_i - (kZ * dY_r + kY * dZ_r) - kZ * kY * val_i;
    const RealType h_zz_i = f_zz_i - 2 * kZ * dZ_r - kZ * kZ * val_i;

    grad_grad_psi[psiIndex][0] = ComplexT(c * h_xx_r - s * h_xx_i, c * h_xx_i + s * h_xx_r);
    grad_grad_psi[psiIndex][1] = ComplexT(c * h_xy_r - s * h_xy_i, c * h_xy_i + s * h_xy_r);
    grad_grad_psi[psiIndex][2] = ComplexT(c * h_xz_r - s * h_xz_i, c * h_xz_i + s * h_xz_r);
    grad_grad_psi[psiIndex][3] = ComplexT(c * h_xy_r - s * h_xy_i, c * h_xy_i + s * h_xy_r);
    grad_grad_psi[psiIndex][4] = ComplexT(c * h_yy_r - s * h_yy_i, c * h_yy_i + s * h_yy_r);
    grad_grad_psi[psiIndex][5] = ComplexT(c * h_yz_r - s * h_yz_i, c * h_yz_i + s * h_yz_r);
    grad_grad_psi[psiIndex][6] = ComplexT(c * h_xz_r - s * h_xz_i, c * h_xz_i + s * h_xz_r);
    grad_grad_psi[psiIndex][7] = ComplexT(c * h_yz_r - s * h_yz_i, c * h_yz_i + s * h_yz_r);
    grad_grad_psi[psiIndex][8] = ComplexT(c * h_zz_r - s * h_zz_i, c * h_zz_i + s * h_zz_r);

    //These are the real and imaginary components of the third SPO derivative.  _xxx denotes
    // third derivative w.r.t. x, _xyz, a derivative with resepect to x,y, and z, and so on.

    const RealType f3_xxx_r = t3_contract(gh000[jr], gh001[jr], gh002[jr], gh011[jr], gh012[jr], gh022[jr], gh111[jr],
                                          gh112[jr], gh122[jr], gh222[jr], g00, g01, g02, g00, g01, g02, g00, g01, g02);
    const RealType f3_xxy_r = t3_contract(gh000[jr], gh001[jr], gh002[jr], gh011[jr], gh012[jr], gh022[jr], gh111[jr],
                                          gh112[jr], gh122[jr], gh222[jr], g00, g01, g02, g00, g01, g02, g10, g11, g12);
    const RealType f3_xxz_r = t3_contract(gh000[jr], gh001[jr], gh002[jr], gh011[jr], gh012[jr], gh022[jr], gh111[jr],
                                          gh112[jr], gh122[jr], gh222[jr], g00, g01, g02, g00, g01, g02, g20, g21, g22);
    const RealType f3_xyy_r = t3_contract(gh000[jr], gh001[jr], gh002[jr], gh011[jr], gh012[jr], gh022[jr], gh111[jr],
                                          gh112[jr], gh122[jr], gh222[jr], g00, g01, g02, g10, g11, g12, g10, g11, g12);
    const RealType f3_xyz_r = t3_contract(gh000[jr], gh001[jr], gh002[jr], gh011[jr], gh012[jr], gh022[jr], gh111[jr],
                                          gh112[jr], gh122[jr], gh222[jr], g00, g01, g02, g10, g11, g12, g20, g21, g22);
    const RealType f3_xzz_r = t3_contract(gh000[jr], gh001[jr], gh002[jr], gh011[jr], gh012[jr], gh022[jr], gh111[jr],
                                          gh112[jr], gh122[jr], gh222[jr], g00, g01, g02, g20, g21, g22, g20, g21, g22);
    const RealType f3_yyy_r = t3_contract(gh000[jr], gh001[jr], gh002[jr], gh011[jr], gh012[jr], gh022[jr], gh111[jr],
                                          gh112[jr], gh122[jr], gh222[jr], g10, g11, g12, g10, g11, g12, g10, g11, g12);
    const RealType f3_yyz_r = t3_contract(gh000[jr], gh001[jr], gh002[jr], gh011[jr], gh012[jr], gh022[jr], gh111[jr],
                                          gh112[jr], gh122[jr], gh222[jr], g10, g11, g12, g10, g11, g12, g20, g21, g22);
    const RealType f3_yzz_r = t3_contract(gh000[jr], gh001[jr], gh002[jr], gh011[jr], gh012[jr], gh022[jr], gh111[jr],
                                          gh112[jr], gh122[jr], gh222[jr], g10, g11, g12, g20, g21, g22, g20, g21, g22);
    const RealType f3_zzz_r = t3_contract(gh000[jr], gh001[jr], gh002[jr], gh011[jr], gh012[jr], gh022[jr], gh111[jr],
                                          gh112[jr], gh122[jr], gh222[jr], g20, g21, g22, g20, g21, g22, g20, g21, g22);

    const RealType f3_xxx_i = t3_contract(gh000[ji], gh001[ji], gh002[ji], gh011[ji], gh012[ji], gh022[ji], gh111[ji],
                                          gh112[ji], gh122[ji], gh222[ji], g00, g01, g02, g00, g01, g02, g00, g01, g02);
    const RealType f3_xxy_i = t3_contract(gh000[ji], gh001[ji], gh002[ji], gh011[ji], gh012[ji], gh022[ji], gh111[ji],
                                          gh112[ji], gh122[ji], gh222[ji], g00, g01, g02, g00, g01, g02, g10, g11, g12);
    const RealType f3_xxz_i = t3_contract(gh000[ji], gh001[ji], gh002[ji], gh011[ji], gh012[ji], gh022[ji], gh111[ji],
                                          gh112[ji], gh122[ji], gh222[ji], g00, g01, g02, g00, g01, g02, g20, g21, g22);
    const RealType f3_xyy_i = t3_contract(gh000[ji], gh001[ji], gh002[ji], gh011[ji], gh012[ji], gh022[ji], gh111[ji],
                                          gh112[ji], gh122[ji], gh222[ji], g00, g01, g02, g10, g11, g12, g10, g11, g12);
    const RealType f3_xyz_i = t3_contract(gh000[ji], gh001[ji], gh002[ji], gh011[ji], gh012[ji], gh022[ji], gh111[ji],
                                          gh112[ji], gh122[ji], gh222[ji], g00, g01, g02, g10, g11, g12, g20, g21, g22);
    const RealType f3_xzz_i = t3_contract(gh000[ji], gh001[ji], gh002[ji], gh011[ji], gh012[ji], gh022[ji], gh111[ji],
                                          gh112[ji], gh122[ji], gh222[ji], g00, g01, g02, g20, g21, g22, g20, g21, g22);
    const RealType f3_yyy_i = t3_contract(gh000[ji], gh001[ji], gh002[ji], gh011[ji], gh012[ji], gh022[ji], gh111[ji],
                                          gh112[ji], gh122[ji], gh222[ji], g10, g11, g12, g10, g11, g12, g10, g11, g12);
    const RealType f3_yyz_i = t3_contract(gh000[ji], gh001[ji], gh002[ji], gh011[ji], gh012[ji], gh022[ji], gh111[ji],
                                          gh112[ji], gh122[ji], gh222[ji], g10, g11, g12, g10, g11, g12, g20, g21, g22);
    const RealType f3_yzz_i = t3_contract(gh000[ji], gh001[ji], gh002[ji], gh011[ji], gh012[ji], gh022[ji], gh111[ji],
                                          gh112[ji], gh122[ji], gh222[ji], g10, g11, g12, g20, g21, g22, g20, g21, g22);
    const RealType f3_zzz_i = t3_contract(gh000[ji], gh001[ji], gh002[ji], gh011[ji], gh012[ji], gh022[ji], gh111[ji],
                                          gh112[ji], gh122[ji], gh222[ji], g20, g21, g22, g20, g21, g22, g20, g21, g22);

    //Here is where we build up the components of the physical hessian gradient, namely, d^3/dx^3(e^{-ik*r}\phi(r)
    const RealType gh_xxx_r = f3_xxx_r + 3 * kX * f_xx_i - 3 * kX * kX * dX_r - kX * kX * kX * val_i;
    const RealType gh_xxx_i = f3_xxx_i - 3 * kX * f_xx_r - 3 * kX * kX * dX_i + kX * kX * kX * val_r;
    const RealType gh_xxy_r =
        f3_xxy_r + (kY * f_xx_i + 2 * kX * f_xy_i) - (kX * kX * dY_r + 2 * kX * kY * dX_r) - kX * kX * kY * val_i;
    const RealType gh_xxy_i =
        f3_xxy_i - (kY * f_xx_r + 2 * kX * f_xy_r) - (kX * kX * dY_i + 2 * kX * kY * dX_i) + kX * kX * kY * val_r;
    const RealType gh_xxz_r =
        f3_xxz_r + (kZ * f_xx_i + 2 * kX * f_xz_i) - (kX * kX * dZ_r + 2 * kX * kZ * dX_r) - kX * kX * kZ * val_i;
    const RealType gh_xxz_i =
        f3_xxz_i - (kZ * f_xx_r + 2 * kX * f_xz_r) - (kX * kX * dZ_i + 2 * kX * kZ * dX_i) + kX * kX * kZ * val_r;
    const RealType gh_xyy_r =
        f3_xyy_r + (2 * kY * f_xy_i + kX * f_yy_i) - (2 * kX * kY * dY_r + kY * kY * dX_r) - kX * kY * kY * val_i;
    const RealType gh_xyy_i =
        f3_xyy_i - (2 * kY * f_xy_r + kX * f_yy_r) - (2 * kX * kY * dY_i + kY * kY * dX_i) + kX * kY * kY * val_r;
    const RealType gh_xyz_r = f3_xyz_r + (kX * f_yz_i + kY * f_xz_i + kZ * f_xy_i) -
        (kX * kY * dZ_r + kY * kZ * dX_r + kZ * kX * dY_r) - kX * kY * kZ * val_i;
    const RealType gh_xyz_i = f3_xyz_i - (kX * f_yz_r + kY * f_xz_r + kZ * f_xy_r) -
        (kX * kY * dZ_i + kY * kZ * dX_i + kZ * kX * dY_i) + kX * kY * kZ * val_r;
    const RealType gh_xzz_r =
        f3_xzz_r + (2 * kZ * f_xz_i + kX * f_zz_i) - (2 * kX * kZ * dZ_r + kZ * kZ * dX_r) - kX * kZ * kZ * val_i;
    const RealType gh_xzz_i =
        f3_xzz_i - (2 * kZ * f_xz_r + kX * f_zz_r) - (2 * kX * kZ * dZ_i + kZ * kZ * dX_i) + kX * kZ * kZ * val_r;
    const RealType gh_yyy_r = f3_yyy_r + 3 * kY * f_yy_i - 3 * kY * kY * dY_r - kY * kY * kY * val_i;
    const RealType gh_yyy_i = f3_yyy_i - 3 * kY * f_yy_r - 3 * kY * kY * dY_i + kY * kY * kY * val_r;
    const RealType gh_yyz_r =
        f3_yyz_r + (kZ * f_yy_i + 2 * kY * f_yz_i) - (kY * kY * dZ_r + 2 * kY * kZ * dY_r) - kY * kY * kZ * val_i;
    const RealType gh_yyz_i =
        f3_yyz_i - (kZ * f_yy_r + 2 * kY * f_yz_r) - (kY * kY * dZ_i + 2 * kY * kZ * dY_i) + kY * kY * kZ * val_r;
    const RealType gh_yzz_r =
        f3_yzz_r + (2 * kZ * f_yz_i + kY * f_zz_i) - (2 * kY * kZ * dZ_r + kZ * kZ * dY_r) - kY * kZ * kZ * val_i;
    const RealType gh_yzz_i =
        f3_yzz_i - (2 * kZ * f_yz_r + kY * f_zz_r) - (2 * kY * kZ * dZ_i + kZ * kZ * dY_i) + kY * kZ * kZ * val_r;
    const RealType gh_zzz_r = f3_zzz_r + 3 * kZ * f_zz_i - 3 * kZ * kZ * dZ_r - kZ * kZ * kZ * val_i;
    const RealType gh_zzz_i = f3_zzz_i - 3 * kZ * f_zz_r - 3 * kZ * kZ * dZ_i + kZ * kZ * kZ * val_r;

    grad_grad_grad_psi[psiIndex][0][0] = ComplexT(c * gh_xxx_r - s * gh_xxx_i, c * gh_xxx_i + s * gh_xxx_r);
    grad_grad_grad_psi[psiIndex][0][1] = ComplexT(c * gh_xxy_r - s * gh_xxy_i, c * gh_xxy_i + s * gh_xxy_r);
    grad_grad_grad_psi[psiIndex][0][2] = ComplexT(c * gh_xxz_r - s * gh_xxz_i, c * gh_xxz_i + s * gh_xxz_r);
    grad_grad_grad_psi[psiIndex][0][3] = ComplexT(c * gh_xxy_r - s * gh_xxy_i, c * gh_xxy_i + s * gh_xxy_r);
    grad_grad_grad_psi[psiIndex][0][4] = ComplexT(c * gh_xyy_r - s * gh_xyy_i, c * gh_xyy_i + s * gh_xyy_r);
    grad_grad_grad_psi[psiIndex][0][5] = ComplexT(c * gh_xyz_r - s * gh_xyz_i, c * gh_xyz_i + s * gh_xyz_r);
    grad_grad_grad_psi[psiIndex][0][6] = ComplexT(c * gh_xxz_r - s * gh_xxz_i, c * gh_xxz_i + s * gh_xxz_r);
    grad_grad_grad_psi[psiIndex][0][7] = ComplexT(c * gh_xyz_r - s * gh_xyz_i, c * gh_xyz_i + s * gh_xyz_r);
    grad_grad_grad_psi[psiIndex][0][8] = ComplexT(c * gh_xzz_r - s * gh_xzz_i, c * gh_xzz_i + s * gh_xzz_r);

    grad_grad_grad_psi[psiIndex][1][0] = ComplexT(c * gh_xxy_r - s * gh_xxy_i, c * gh_xxy_i + s * gh_xxy_r);
    grad_grad_grad_psi[psiIndex][1][1] = ComplexT(c * gh_xyy_r - s * gh_xyy_i, c * gh_xyy_i + s * gh_xyy_r);
    grad_grad_grad_psi[psiIndex][1][2] = ComplexT(c * gh_xyz_r - s * gh_xyz_i, c * gh_xyz_i + s * gh_xyz_r);
    grad_grad_grad_psi[psiIndex][1][3] = ComplexT(c * gh_xyy_r - s * gh_xyy_i, c * gh_xyy_i + s * gh_xyy_r);
    grad_grad_grad_psi[psiIndex][1][4] = ComplexT(c * gh_yyy_r - s * gh_yyy_i, c * gh_yyy_i + s * gh_yyy_r);
    grad_grad_grad_psi[psiIndex][1][5] = ComplexT(c * gh_yyz_r - s * gh_yyz_i, c * gh_yyz_i + s * gh_yyz_r);
    grad_grad_grad_psi[psiIndex][1][6] = ComplexT(c * gh_xyz_r - s * gh_xyz_i, c * gh_xyz_i + s * gh_xyz_r);
    grad_grad_grad_psi[psiIndex][1][7] = ComplexT(c * gh_yyz_r - s * gh_yyz_i, c * gh_yyz_i + s * gh_yyz_r);
    grad_grad_grad_psi[psiIndex][1][8] = ComplexT(c * gh_yzz_r - s * gh_yzz_i, c * gh_yzz_i + s * gh_yzz_r);


    grad_grad_grad_psi[psiIndex][2][0] = ComplexT(c * gh_xxz_r - s * gh_xxz_i, c * gh_xxz_i + s * gh_xxz_r);
    grad_grad_grad_psi[psiIndex][2][1] = ComplexT(c * gh_xyz_r - s * gh_xyz_i, c * gh_xyz_i + s * gh_xyz_r);
    grad_grad_grad_psi[psiIndex][2][2] = ComplexT(c * gh_xzz_r - s * gh_xzz_i, c * gh_xzz_i + s * gh_xzz_r);
    grad_grad_grad_psi[psiIndex][2][3] = ComplexT(c * gh_xyz_r - s * gh_xyz_i, c * gh_xyz_i + s * gh_xyz_r);
    grad_grad_grad_psi[psiIndex][2][4] = ComplexT(c * gh_yyz_r - s * gh_yyz_i, c * gh_yyz_i + s * gh_yyz_r);
    grad_grad_grad_psi[psiIndex][2][5] = ComplexT(c * gh_yzz_r - s * gh_yzz_i, c * gh_yzz_i + s * gh_yzz_r);
    grad_grad_grad_psi[psiIndex][2][6] = ComplexT(c * gh_xzz_r - s * gh_xzz_i, c * gh_xzz_i + s * gh_xzz_r);
    grad_grad_grad_psi[psiIndex][2][7] = ComplexT(c * gh_yzz_r - s * gh_yzz_i, c * gh_yzz_i + s * gh_yzz_r);
    grad_grad_grad_psi[psiIndex][2][8] = ComplexT(c * gh_zzz_r - s * gh_zzz_i, c * gh_zzz_i + s * gh_zzz_r);
  }
}

template<class T>
void SplineC2CT<T>::evaluateVGHGH(const ParticleSetT<T>& P,
                                  const int iat,
                                  ValueVector& psi,
                                  GradVector& dpsi,
                                  HessVector& grad_grad_psi,
                                  GGGVector& grad_grad_grad_psi)
{
  const PointType& r = P.activeR(iat);
  PointType ru(PrimLattice.toUnit_floor(r));
#pragma omp parallel
  {
    int first, last;
    // Factor of 2 because psi is complex and the spline storage and evaluation uses a real type
    FairDivideAligned(2 * psi.size(), getAlignment<T>(), omp_get_num_threads(), omp_get_thread_num(), first, last);

    spline2::evaluate3d_vghgh(SplineInst->getSplinePtr(), ru, myV, myG, myH, mygH, first, last);
    assign_vghgh(r, psi, dpsi, grad_grad_psi, grad_grad_grad_psi, first / 2, last / 2);
  }
}

template class SplineC2CT<std::complex<float>>;
template class SplineC2CT<std::complex<double>>;

} // namespace qmcplusplus
