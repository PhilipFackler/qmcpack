//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2019 QMCPACK developers.
//
// File developed by: Ye Luo, yeluo@anl.gov, Argonne National Laboratory
//
// File created by: Ye Luo, yeluo@anl.gov, Argonne National Laboratory
//
//////////////////////////////////////////////////////////////////////////////////////


/** @file HybridRepCplx.h
 *
 * hold HybridRepCplx
 */
#ifndef QMCPLUSPLUS_HYBRIDREP_CPLX_H
#define QMCPLUSPLUS_HYBRIDREP_CPLX_H

#include <QMCWaveFunctions/BsplineFactory/HybridRepCenterOrbitals.h>
namespace qmcplusplus
{
/** hybrid representation orbitals combining B-spline orbitals on a grid and atomic centered orbitals.
 * @tparam SPLINEBASE B-spline orbital class.
 *
 * Only works with SPLINEBASE class containing complex splines
 */
template<typename SPLINEBASE>
struct HybridRepCplx : public SPLINEBASE, private HybridRepCenterOrbitals<typename SPLINEBASE::DataType>
{
  using HYBRIDBASE       = HybridRepCenterOrbitals<typename SPLINEBASE::DataType>;
  using ST               = typename SPLINEBASE::DataType;
  using PointType        = typename SPLINEBASE::PointType;
  using SingleSplineType = typename SPLINEBASE::SingleSplineType;
  using RealType         = typename SPLINEBASE::RealType;
  // types for evaluation results
  using typename SPLINEBASE::GGGVector_t;
  using typename SPLINEBASE::GradVector_t;
  using typename SPLINEBASE::HessVector_t;
  using typename SPLINEBASE::ValueType;
  using typename SPLINEBASE::ValueVector_t;

  ValueVector_t psi_AO, d2psi_AO;
  GradVector_t dpsi_AO;
  Matrix<ST, aligned_allocator<ST>> multi_myV;

  using SPLINEBASE::myG;
  using SPLINEBASE::myH;
  using SPLINEBASE::myL;
  using SPLINEBASE::myV;

  HybridRepCplx()
  {
    this->className = "Hybrid" + this->className;
    this->KeyWord   = "Hybrid" + this->KeyWord;
  }

  virtual SPOSet* makeClone() const override { return new HybridRepCplx(*this); }

  inline void resizeStorage(size_t n, size_t nvals)
  {
    SPLINEBASE::resizeStorage(n, nvals);
    HYBRIDBASE::resizeStorage(myV.size());
  }

  void bcast_tables(Communicate* comm)
  {
    SPLINEBASE::bcast_tables(comm);
    HYBRIDBASE::bcast_tables(comm);
  }

  void gather_tables(Communicate* comm)
  {
    SPLINEBASE::gather_tables(comm);
    HYBRIDBASE::gather_atomic_tables(comm, SPLINEBASE::offset);
  }

  bool read_splines(hdf_archive& h5f) { return HYBRIDBASE::read_splines(h5f) && SPLINEBASE::read_splines(h5f); }

  bool write_splines(hdf_archive& h5f) { return HYBRIDBASE::write_splines(h5f) && SPLINEBASE::write_splines(h5f); }

  inline void flush_zero()
  {
    //SPLINEBASE::flush_zero();
    HYBRIDBASE::flush_zero();
  }

  void evaluateValue(const ParticleSet& P, const int iat, ValueVector_t& psi) override
  {
    const RealType smooth_factor = HYBRIDBASE::evaluate_v(P, iat, myV);
    const RealType cone(1);
    if (smooth_factor < 0)
    {
      SPLINEBASE::evaluateValue(P, iat, psi);
    }
    else if (smooth_factor == cone)
    {
      const PointType& r = P.activeR(iat);
      SPLINEBASE::assign_v(r, myV, psi, 0, myV.size() / 2);
    }
    else
    {
      const PointType& r = P.activeR(iat);
      psi_AO.resize(psi.size());
      SPLINEBASE::assign_v(r, myV, psi_AO, 0, myV.size() / 2);
      SPLINEBASE::evaluateValue(P, iat, psi);
      HYBRIDBASE::interpolate_buffer_v(psi, psi_AO);
    }
  }


  void evaluateDetRatios(const VirtualParticleSet& VP,
                         ValueVector_t& psi,
                         const ValueVector_t& psiinv,
                         std::vector<ValueType>& ratios) override
  {
    if (VP.isOnSphere())
    {
      // resize scratch space
      psi_AO.resize(psi.size());
      if (multi_myV.rows() < VP.getTotalNum())
        multi_myV.resize(VP.getTotalNum(), myV.size());
      const RealType smooth_factor = HYBRIDBASE::evaluateValuesC2X(VP, multi_myV);
      const RealType cone(1);
      for (int iat = 0; iat < VP.getTotalNum(); ++iat)
      {
        if (smooth_factor < 0)
          SPLINEBASE::evaluateValue(VP, iat, psi);
        else if (smooth_factor == cone)
        {
          const PointType& r = VP.R[iat];
          Vector<ST, aligned_allocator<ST>> myV_one(multi_myV[iat], myV.size());
          SPLINEBASE::assign_v(r, myV_one, psi, 0, myV.size() / 2);
        }
        else
        {
          const PointType& r = VP.R[iat];
          Vector<ST, aligned_allocator<ST>> myV_one(multi_myV[iat], myV.size());
          SPLINEBASE::assign_v(r, myV_one, psi_AO, 0, myV.size() / 2);
          SPLINEBASE::evaluateValue(VP, iat, psi);
          HYBRIDBASE::interpolate_buffer_v(psi, psi_AO);
        }
        ratios[iat] = simd::dot(psi.data(), psiinv.data(), psi.size());
      }
    }
    else
    {
      for (int iat = 0; iat < VP.getTotalNum(); ++iat)
      {
        evaluateValue(VP, iat, psi);
        ratios[iat] = simd::dot(psi.data(), psiinv.data(), psi.size());
      }
    }
  }

  void evaluateVGL(const ParticleSet& P,
                   const int iat,
                   ValueVector_t& psi,
                   GradVector_t& dpsi,
                   ValueVector_t& d2psi) override
  {
    const RealType smooth_factor = HYBRIDBASE::evaluate_vgl(P, iat, myV, myG, myL);
    const RealType cone(1);
    if (smooth_factor < 0)
    {
      SPLINEBASE::evaluateVGL(P, iat, psi, dpsi, d2psi);
    }
    else if (smooth_factor == cone)
    {
      const PointType& r = P.activeR(iat);
      SPLINEBASE::assign_vgl_from_l(r, psi, dpsi, d2psi);
    }
    else
    {
      const PointType& r = P.activeR(iat);
      psi_AO.resize(psi.size());
      dpsi_AO.resize(psi.size());
      d2psi_AO.resize(psi.size());
      SPLINEBASE::assign_vgl_from_l(r, psi_AO, dpsi_AO, d2psi_AO);
      SPLINEBASE::evaluateVGL(P, iat, psi, dpsi, d2psi);
      HYBRIDBASE::interpolate_buffer_vgl(psi, dpsi, d2psi, psi_AO, dpsi_AO, d2psi_AO);
    }
  }

  void evaluateVGH(const ParticleSet& P,
                   const int iat,
                   ValueVector_t& psi,
                   GradVector_t& dpsi,
                   HessVector_t& grad_grad_psi) override
  {
    APP_ABORT("HybridRepCplx::evaluate_vgh not implemented!");
    if (HYBRIDBASE::evaluate_vgh(P, iat, myV, myG, myH))
    {
      const PointType& r = P.activeR(iat);
      SPLINEBASE::assign_vgh(r, psi, dpsi, grad_grad_psi, 0, myV.size() / 2);
    }
    else
      SPLINEBASE::evaluateVGH(P, iat, psi, dpsi, grad_grad_psi);
  }

  void evaluateVGHGH(const ParticleSet& P,
                     const int iat,
                     ValueVector_t& psi,
                     GradVector_t& dpsi,
                     HessVector_t& grad_grad_psi,
                     GGGVector_t& grad_grad_grad_psi) override
  {
    APP_ABORT("HybridRepCplx::evaluate_vghgh not implemented!");
  }

  template<class BSPLINESPO> friend class HybridRepSetReader;
};

} // namespace qmcplusplus
#endif
