//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#include "RankTwoTensor.h"

// MOOSE includes
#include "MooseEnum.h"
#include "ColumnMajorMatrix.h"
#include "MooseRandom.h"
#include "RankFourTensor.h"
#include "Conversion.h"
#include "MooseArray.h"

#include "libmesh/libmesh.h"
#include "libmesh/tensor_value.h"
#include "libmesh/vector_value.h"
#include "libmesh/utility.h"

// PETSc includes
#include <petscblaslapack.h>

// C++ includes
#include <iomanip>
#include <ostream>
#include <vector>
#include <array>

// Eigen includes
#include <Eigen/Core>
#include <Eigen/Eigenvalues>

namespace Eigen
{
namespace internal
{
template <>
struct cast_impl<ADReal, int>
{
  static inline int run(const ADReal & x) { return static_cast<int>(MetaPhysicL::raw_value(x)); }
};
} // namespace internal
} // namespace Eigen

template <typename T>
constexpr Real RankTwoTensorTempl<T>::identityCoords[];

namespace MathUtils
{
template <>
void mooseSetToZero<RankTwoTensor>(RankTwoTensor & v);
template <>
void mooseSetToZero<ADRankTwoTensor>(ADRankTwoTensor & v);
}

template <typename T>
MooseEnum
RankTwoTensorTempl<T>::fillMethodEnum()
{
  return MooseEnum("autodetect=0 isotropic1=1 diagonal3=3 symmetric6=6 general=9", "autodetect");
}

template <typename T>
RankTwoTensorTempl<T>::RankTwoTensorTempl()
{
  mooseAssert(N == 3, "RankTwoTensorTempl is currently only tested for 3 dimensions.");

  for (unsigned int i = 0; i < N2; i++)
    this->_coords[i] = 0.0;
}

template <typename T>
RankTwoTensorTempl<T>::RankTwoTensorTempl(const InitMethod init)
{
  switch (init)
  {
    case initNone:
      break;

    case initIdentity:
      this->zero();
      for (auto i : make_range(N))
        (*this)(i, i) = 1.0;
      break;

    default:
      mooseError("Unknown RankTwoTensorTempl initialization pattern.");
  }
}

/// TODO: deprecate this method in favor of initializeFromRows
template <typename T>
RankTwoTensorTempl<T>::RankTwoTensorTempl(const TypeVector<T> & row1,
                                          const TypeVector<T> & row2,
                                          const TypeVector<T> & row3)
{
  mooseDeprecated(
      "This constructor is deprecated in favor of RankTwoTensorTempl<T>::initializeFromRows");

  // Initialize the Tensor matrix from the passed in vectors
  for (auto i : make_range(N))
    this->_coords[i] = row1(i);

  for (auto i : make_range(N))
    this->_coords[N + i] = row2(i);

  const unsigned int two_n = N * 2;
  for (auto i : make_range(N))
    this->_coords[two_n + i] = row3(i);
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::initializeFromRows(const TypeVector<T> & row0,
                                          const TypeVector<T> & row1,
                                          const TypeVector<T> & row2)
{
  return RankTwoTensorTempl<T>(
      row0(0), row1(0), row2(0), row0(1), row1(1), row2(1), row0(2), row1(2), row2(2));
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::initializeFromColumns(const TypeVector<T> & col0,
                                             const TypeVector<T> & col1,
                                             const TypeVector<T> & col2)
{
  return RankTwoTensorTempl<T>(
      col0(0), col0(1), col0(2), col1(0), col1(1), col1(2), col2(0), col2(1), col2(2));
}

template <typename T>
RankTwoTensorTempl<T>::RankTwoTensorTempl(T S11, T S22, T S33, T S23, T S13, T S12)
{
  (*this)(0, 0) = S11;
  (*this)(1, 1) = S22;
  (*this)(2, 2) = S33;
  (*this)(1, 2) = (*this)(2, 1) = S23;
  (*this)(0, 2) = (*this)(2, 0) = S13;
  (*this)(0, 1) = (*this)(1, 0) = S12;
}

template <typename T>
RankTwoTensorTempl<T>::RankTwoTensorTempl(
    T S11, T S21, T S31, T S12, T S22, T S32, T S13, T S23, T S33)
{
  (*this)(0, 0) = S11;
  (*this)(1, 0) = S21;
  (*this)(2, 0) = S31;
  (*this)(0, 1) = S12;
  (*this)(1, 1) = S22;
  (*this)(2, 1) = S32;
  (*this)(0, 2) = S13;
  (*this)(1, 2) = S23;
  (*this)(2, 2) = S33;
}

template <typename T>
void
RankTwoTensorTempl<T>::fillFromInputVector(const std::vector<T> & input, FillMethod fill_method)
{
  if (fill_method != autodetect && fill_method != input.size())
    mooseError("Expected an input vector size of ", fill_method, " to fill the RankTwoTensorTempl");

  switch (input.size())
  {
    case 1:
      this->zero();
      (*this)(0, 0) = input[0]; // S11
      (*this)(1, 1) = input[0]; // S22
      (*this)(2, 2) = input[0]; // S33
      break;

    case 3:
      this->zero();
      (*this)(0, 0) = input[0]; // S11
      (*this)(1, 1) = input[1]; // S22
      (*this)(2, 2) = input[2]; // S33
      break;

    case 6:
      (*this)(0, 0) = input[0];                 // S11
      (*this)(1, 1) = input[1];                 // S22
      (*this)(2, 2) = input[2];                 // S33
      (*this)(1, 2) = (*this)(2, 1) = input[3]; // S23
      (*this)(0, 2) = (*this)(2, 0) = input[4]; // S13
      (*this)(0, 1) = (*this)(1, 0) = input[5]; // S12
      break;

    case 9:
      (*this)(0, 0) = input[0]; // S11
      (*this)(1, 0) = input[1]; // S21
      (*this)(2, 0) = input[2]; // S31
      (*this)(0, 1) = input[3]; // S12
      (*this)(1, 1) = input[4]; // S22
      (*this)(2, 1) = input[5]; // S32
      (*this)(0, 2) = input[6]; // S13
      (*this)(1, 2) = input[7]; // S23
      (*this)(2, 2) = input[8]; // S33
      break;

    default:
      mooseError("Please check the number of entries in the input vector for building "
                 "a RankTwoTensorTempl. It must be 1, 3, 6, or 9");
  }
}

template <typename T>
void
RankTwoTensorTempl<T>::fillFromScalarVariable(const VariableValue & scalar_variable)
{
  switch (scalar_variable.size())
  {
    case 1:
      this->zero();
      (*this)(0, 0) = scalar_variable[0]; // S11
      break;

    case 3:
      this->zero();
      (*this)(0, 0) = scalar_variable[0];                 // S11
      (*this)(1, 1) = scalar_variable[1];                 // S22
      (*this)(0, 1) = (*this)(1, 0) = scalar_variable[2]; // S12
      break;

    case 6:
      (*this)(0, 0) = scalar_variable[0];                 // S11
      (*this)(1, 1) = scalar_variable[1];                 // S22
      (*this)(2, 2) = scalar_variable[2];                 // S33
      (*this)(1, 2) = (*this)(2, 1) = scalar_variable[3]; // S23
      (*this)(0, 2) = (*this)(2, 0) = scalar_variable[4]; // S13
      (*this)(0, 1) = (*this)(1, 0) = scalar_variable[5]; // S12
      break;

    default:
      mooseError("Only FIRST, THIRD, or SIXTH order scalar variable can be used to build "
                 "a RankTwoTensorTempl.");
  }
}

template <typename T>
TypeVector<T>
RankTwoTensorTempl<T>::column(const unsigned int c) const
{
  VectorValue<T> result;

  for (auto i : make_range(N))
    result(i) = (*this)(i, c);

  return result;
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::rotated(const RankTwoTensorTempl<T> & R) const
{
  RankTwoTensorTempl<T> result(*this);
  result.rotate(R);
  return result;
}

template <typename T>
void
RankTwoTensorTempl<T>::rotate(const RankTwoTensorTempl<T> & R)
{
  RankTwoTensorTempl<T> temp;
  for (auto i : make_range(N))
  {
    const auto i1 = i * N;
    for (auto j : make_range(N))
    {
      // tmp += R(i,k)*R(j,l)*(*this)(k,l);
      // clang-format off
      const auto j1 = j * N;
      T tmp = R._coords[i1 + 0] * R._coords[j1 + 0] * (*this)(0, 0) +
                 R._coords[i1 + 0] * R._coords[j1 + 1] * (*this)(0, 1) +
                 R._coords[i1 + 0] * R._coords[j1 + 2] * (*this)(0, 2) +
                 R._coords[i1 + 1] * R._coords[j1 + 0] * (*this)(1, 0) +
                 R._coords[i1 + 1] * R._coords[j1 + 1] * (*this)(1, 1) +
                 R._coords[i1 + 1] * R._coords[j1 + 2] * (*this)(1, 2) +
                 R._coords[i1 + 2] * R._coords[j1 + 0] * (*this)(2, 0) +
                 R._coords[i1 + 2] * R._coords[j1 + 1] * (*this)(2, 1) +
                 R._coords[i1 + 2] * R._coords[j1 + 2] * (*this)(2, 2);
      // clang-format on
      temp._coords[i1 + j] = tmp;
    }
  }
  for (unsigned int i = 0; i < N2; i++)
    this->_coords[i] = temp._coords[i];
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::rotateXyPlane(T a)
{
  T c = std::cos(a);
  T s = std::sin(a);
  T x = (*this)(0, 0) * c * c + (*this)(1, 1) * s * s + 2.0 * (*this)(0, 1) * c * s;
  T y = (*this)(0, 0) * s * s + (*this)(1, 1) * c * c - 2.0 * (*this)(0, 1) * c * s;
  T xy = ((*this)(1, 1) - (*this)(0, 0)) * c * s + (*this)(0, 1) * (c * c - s * s);

  RankTwoTensorTempl<T> b(*this);

  b(0, 0) = x;
  b(1, 1) = y;
  b(1, 0) = b(0, 1) = xy;

  return b;
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::transpose() const
{
  return TensorValue<T>::transpose();
}

template <typename T>
RankTwoTensorTempl<T> &
RankTwoTensorTempl<T>::operator=(const RankTwoTensorTempl<T> & a)
{
  TensorValue<T>::operator=(a);
  return *this;
}

template <typename T>
RankTwoTensorTempl<T> &
RankTwoTensorTempl<T>::operator+=(const RankTwoTensorTempl<T> & a)
{
  TensorValue<T>::operator+=(a);
  return *this;
}

template <typename T>
RankTwoTensorTempl<T> &
RankTwoTensorTempl<T>::operator-=(const RankTwoTensorTempl<T> & a)
{
  TensorValue<T>::operator-=(a);
  return *this;
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::operator-() const
{
  return TensorValue<T>::operator-();
}

template <typename T>
RankTwoTensorTempl<T> &
RankTwoTensorTempl<T>::operator*=(const T & a)
{
  TensorValue<T>::operator*=(a);
  return *this;
}

template <typename T>
RankTwoTensorTempl<T> &
RankTwoTensorTempl<T>::operator/=(const T & a)
{
  TensorValue<T>::operator/=(a);
  return *this;
}

template <typename T>
RankTwoTensorTempl<T> &
RankTwoTensorTempl<T>::operator*=(const TypeTensor<T> & a)
{
  *this = *this * a;
  return *this;
}

template <typename T>
bool
RankTwoTensorTempl<T>::operator==(const RankTwoTensorTempl<T> & a) const
{
  for (auto i : make_range(N))
    for (auto j : make_range(N))
      if (!MooseUtils::absoluteFuzzyEqual((*this)(i, j), a(i, j)))
        return false;

  return true;
}

template <typename T>
RankTwoTensorTempl<T> &
RankTwoTensorTempl<T>::operator=(const ColumnMajorMatrixTempl<T> & a)
{
  if (a.n() != N || a.m() != N)
    mooseError("Dimensions of ColumnMajorMatrixTempl<T> are incompatible with RankTwoTensorTempl");

  const T * cmm_rawdata = a.rawData();
  for (auto i : make_range(N))
    for (auto j : make_range(N))
      this->_coords[i * N + j] = cmm_rawdata[i + j * N];

  return *this;
}

template <typename T>
T
RankTwoTensorTempl<T>::doubleContraction(const RankTwoTensorTempl<T> & b) const
{
  // deprecate this!
  return TensorValue<T>::contract(b);
}

template <typename T>
RankFourTensorTempl<T>
RankTwoTensorTempl<T>::outerProduct(const RankTwoTensorTempl<T> & b) const
{
  RankFourTensorTempl<T> result;

  unsigned int index = 0;
  for (unsigned int ij = 0; ij < N2; ++ij)
  {
    const T & a = this->_coords[ij];
    for (unsigned int kl = 0; kl < N2; ++kl)
      result._vals[index++] = a * b._coords[kl];
  }

  return result;
}

template <typename T>
RankFourTensorTempl<T>
RankTwoTensorTempl<T>::mixedProductIkJl(const RankTwoTensorTempl<T> & b) const
{
  RankFourTensorTempl<T> result;

  unsigned int index = 0;
  for (auto i : make_range(N))
    for (auto j : make_range(N))
      for (auto k : make_range(N))
      {
        const T & a = (*this)(i, k);
        for (auto l : make_range(N))
          result._vals[index++] = a * b(j, l);
      }

  return result;
}

template <typename T>
RankFourTensorTempl<T>
RankTwoTensorTempl<T>::mixedProductIlJk(const RankTwoTensorTempl<T> & b) const
{
  RankFourTensorTempl<T> result;

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      for (auto k : make_range(N))
        for (auto l : make_range(N))
          result(i, j, k, l) = (*this)(i, l) * b(j, k);

  return result;
}

template <typename T>
RankFourTensorTempl<T>
RankTwoTensorTempl<T>::mixedProductJkIl(const RankTwoTensorTempl<T> & b) const
{
  RankFourTensorTempl<T> result;

  unsigned int index = 0;
  for (auto i : make_range(N))
    for (auto j : make_range(N))
      for (auto k : make_range(N))
      {
        const T & a = (*this)(j, k);
        for (auto l : make_range(N))
          result._vals[index++] = a * b(i, l);
      }

  return result;
}

template <typename T>
RankFourTensorTempl<T>
RankTwoTensorTempl<T>::mixedProductIjJklm(const RankFourTensorTempl<T> & b) const
{
  RankFourTensorTempl<T> result;

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      for (auto k : make_range(N))
        for (auto l : make_range(N))
          for (auto m : make_range(N))
            result(i, k, l, m) += (*this)(i, j) * b(j, k, l, m);

  return result;
}

template <typename T>
RankFourTensorTempl<T>
RankTwoTensorTempl<T>::mixedProductJmIjkl(const RankFourTensorTempl<T> & b) const
{
  RankFourTensorTempl<T> result;

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      for (auto k : make_range(N))
        for (auto l : make_range(N))
          for (auto m : make_range(N))
            result(i, k, l, m) += (*this)(j, m) * b(i, j, k, l);

  return result;
}

template <typename T>
RankFourTensorTempl<T>
RankTwoTensorTempl<T>::mixedProductJkIjlm(const RankFourTensorTempl<T> & b) const
{
  RankFourTensorTempl<T> result;

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      for (auto k : make_range(N))
        for (auto l : make_range(N))
          for (auto m : make_range(N))
            result(i, k, l, m) += (*this)(j, k) * b(i, j, l, m);

  return result;
}

template <typename T>
RankThreeTensorTempl<T>
RankTwoTensorTempl<T>::mixedProductIjJkl(const RankThreeTensorTempl<T> & b) const
{
  RankThreeTensorTempl<T> result;

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      for (auto k : make_range(N))
        for (auto l : make_range(N))
          result(i, k, l) += (*this)(i, j) * b(j, k, l);

  return result;
}

template <typename T>
RankThreeTensorTempl<T>
RankTwoTensorTempl<T>::mixedProductJkI(const VectorValue<T> & b) const
{
  RankThreeTensorTempl<T> result;

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      for (auto k : make_range(N))
        result(i, j, k) += (*this)(j, k) * b(i);

  return result;
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::deviatoric() const
{
  RankTwoTensorTempl<T> deviatoric(*this);
  deviatoric.addIa(-1.0 / 3.0 * this->tr()); // actually construct deviatoric part
  return deviatoric;
}

template <typename T>
T
RankTwoTensorTempl<T>::generalSecondInvariant() const
{
  // clang-format off
  T result = (*this)(0, 0) * (*this)(1, 1) +
                (*this)(0, 0) * (*this)(2, 2) +
                (*this)(1, 1) * (*this)(2, 2) -
                (*this)(0, 1) * (*this)(1, 0) -
                (*this)(0, 2) * (*this)(2, 0) -
                (*this)(1, 2) * (*this)(2, 1);
  // clang-format on
  return result;
}

template <typename T>
T
RankTwoTensorTempl<T>::secondInvariant() const
{
  T result = 0.0;

  // RankTwoTensorTempl<T> deviatoric(*this);
  // deviatoric.addIa(-1.0/3.0 * this->tr()); // actually construct deviatoric part
  // result = 0.5*(deviatoric + deviatoric.transpose()).doubleContraction(deviatoric +
  // deviatoric.transpose());
  result = Utility::pow<2>((*this)(0, 0) - (*this)(1, 1)) / 6.0;
  result += Utility::pow<2>((*this)(0, 0) - (*this)(2, 2)) / 6.0;
  result += Utility::pow<2>((*this)(1, 1) - (*this)(2, 2)) / 6.0;
  result += Utility::pow<2>((*this)(0, 1) + (*this)(1, 0)) / 4.0;
  result += Utility::pow<2>((*this)(0, 2) + (*this)(2, 0)) / 4.0;
  result += Utility::pow<2>((*this)(1, 2) + (*this)(2, 1)) / 4.0;
  return result;
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::dsecondInvariant() const
{
  return 0.5 * (deviatoric() + deviatoric().transpose());
}

template <typename T>
RankFourTensorTempl<T>
RankTwoTensorTempl<T>::d2secondInvariant() const
{
  RankFourTensorTempl<T> result;

  unsigned int index = 0;
  for (auto i : make_range(N))
    for (auto j : make_range(N))
      for (auto k : make_range(N))
        for (auto l : make_range(N))
          result._vals[index++] = 0.5 * (i == k) * (j == l) + 0.5 * (i == l) * (j == k) -
                                  (1.0 / 3.0) * (i == j) * (k == l);

  return result;
}

template <typename T>
T
RankTwoTensorTempl<T>::trace() const
{
  // deprecate this!
  return this->tr();
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::inverse() const
{
  return TensorValue<T>::inverse();
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::dtrace() const
{
  return RankTwoTensorTempl<T>(1, 0, 0, 0, 1, 0, 0, 0, 1);
}

template <typename T>
T
RankTwoTensorTempl<T>::thirdInvariant() const
{
  RankTwoTensorTempl<T> s = 0.5 * deviatoric();
  s += s.transpose();

  T result = 0.0;

  result = s(0, 0) * (s(1, 1) * s(2, 2) - s(2, 1) * s(1, 2));
  result -= s(1, 0) * (s(0, 1) * s(2, 2) - s(2, 1) * s(0, 2));
  result += s(2, 0) * (s(0, 1) * s(1, 2) - s(1, 1) * s(0, 2));

  return result;
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::dthirdInvariant() const
{
  RankTwoTensorTempl<T> s = 0.5 * deviatoric();
  s += s.transpose();

  RankTwoTensorTempl<T> d;
  T sec_over_three = secondInvariant() / 3.0;

  d(0, 0) = s(1, 1) * s(2, 2) - s(2, 1) * s(1, 2) + sec_over_three;
  d(0, 1) = s(2, 0) * s(1, 2) - s(1, 0) * s(2, 2);
  d(0, 2) = s(1, 0) * s(2, 1) - s(2, 0) * s(1, 1);
  d(1, 0) = s(2, 1) * s(0, 2) - s(0, 1) * s(2, 2);
  d(1, 1) = s(0, 0) * s(2, 2) - s(2, 0) * s(0, 2) + sec_over_three;
  d(1, 2) = s(2, 0) * s(0, 1) - s(0, 0) * s(2, 1);
  d(2, 0) = s(0, 1) * s(1, 2) - s(1, 1) * s(0, 2);
  d(2, 1) = s(1, 0) * s(0, 2) - s(0, 0) * s(1, 2);
  d(2, 2) = s(0, 0) * s(1, 1) - s(1, 0) * s(0, 1) + sec_over_three;

  return d;
}

template <typename T>
RankFourTensorTempl<T>
RankTwoTensorTempl<T>::d2thirdInvariant() const
{
  RankTwoTensorTempl<T> s = 0.5 * deviatoric();
  s += s.transpose();

  RankFourTensorTempl<T> d2;
  unsigned int index = 0;
  for (auto i : make_range(N))
    for (auto j : make_range(N))
      for (auto k : make_range(N))
        for (auto l : make_range(N))
        {
          d2._vals[index++] = Real(i == j) * s(k, l) / 3.0 + Real(k == l) * s(i, j) / 3.0;
          // for (auto a: make_range(N))
          //  for (auto b: make_range(N))
          //    d2(i, j, k, l) += 0.5*(PermutationTensor::eps(i, k, a)*PermutationTensor::eps(j, l,
          //    b) + PermutationTensor::eps(i, l, a)*PermutationTensor::eps(j, k, b))*s(a, b);
        }

  // I'm not sure which is more readable: the above
  // PermutationTensor stuff, or the stuff below.
  // Anyway, they yield the same result, and so i leave
  // both of them here to enlighten you!

  d2(0, 0, 1, 1) += s(2, 2);
  d2(0, 0, 1, 2) -= s(2, 1);
  d2(0, 0, 2, 1) -= s(1, 2);
  d2(0, 0, 2, 2) += s(1, 1);

  d2(0, 1, 0, 1) -= s(2, 2) / 2.0;
  d2(0, 1, 1, 0) -= s(2, 2) / 2.0;
  d2(0, 1, 0, 2) += s(1, 2) / 2.0;
  d2(0, 1, 2, 0) += s(1, 2) / 2.0;
  d2(0, 1, 1, 2) += s(2, 0) / 2.0;
  d2(0, 1, 2, 1) += s(2, 0) / 2.0;
  d2(0, 1, 2, 2) -= s(1, 0);

  d2(0, 2, 0, 1) += s(2, 1) / 2.0;
  d2(0, 2, 1, 0) += s(2, 1) / 2.0;
  d2(0, 2, 0, 2) -= s(1, 1) / 2.0;
  d2(0, 2, 2, 0) -= s(1, 1) / 2.0;
  d2(0, 2, 1, 1) -= s(2, 0);
  d2(0, 2, 1, 2) += s(1, 0) / 2.0;
  d2(0, 2, 2, 1) += s(1, 0) / 2.0;

  d2(1, 0, 0, 1) -= s(2, 2) / 2.0;
  d2(1, 0, 1, 0) -= s(2, 2) / 2.0;
  d2(1, 0, 0, 2) += s(1, 2) / 2.0;
  d2(1, 0, 2, 0) += s(1, 2) / 2.0;
  d2(1, 0, 1, 2) += s(2, 0) / 2.0;
  d2(1, 0, 2, 1) += s(2, 0) / 2.0;
  d2(1, 0, 2, 2) -= s(1, 0);

  d2(1, 1, 0, 0) += s(2, 2);
  d2(1, 1, 0, 2) -= s(2, 0);
  d2(1, 1, 2, 0) -= s(2, 0);
  d2(1, 1, 2, 2) += s(0, 0);

  d2(1, 2, 0, 0) -= s(2, 1);
  d2(1, 2, 0, 1) += s(2, 0) / 2.0;
  d2(1, 2, 1, 0) += s(2, 0) / 2.0;
  d2(1, 2, 0, 2) += s(0, 1) / 2.0;
  d2(1, 2, 2, 0) += s(0, 1) / 2.0;
  d2(1, 2, 1, 2) -= s(0, 0) / 2.0;
  d2(1, 2, 2, 1) -= s(0, 0) / 2.0;

  d2(2, 0, 0, 1) += s(2, 1) / 2.0;
  d2(2, 0, 1, 0) += s(2, 1) / 2.0;
  d2(2, 0, 0, 2) -= s(1, 1) / 2.0;
  d2(2, 0, 2, 0) -= s(1, 1) / 2.0;
  d2(2, 0, 1, 1) -= s(2, 0);
  d2(2, 0, 1, 2) += s(1, 0) / 2.0;
  d2(2, 0, 2, 1) += s(1, 0) / 2.0;

  d2(2, 1, 0, 0) -= s(2, 1);
  d2(2, 1, 0, 1) += s(2, 0) / 2.0;
  d2(2, 1, 1, 0) += s(2, 0) / 2.0;
  d2(2, 1, 0, 2) += s(0, 1) / 2.0;
  d2(2, 1, 2, 0) += s(0, 1) / 2.0;
  d2(2, 1, 1, 2) -= s(0, 0) / 2.0;
  d2(2, 1, 2, 1) -= s(0, 0) / 2.0;

  d2(2, 2, 0, 0) += s(1, 1);
  d2(2, 2, 0, 1) -= s(1, 0);
  d2(2, 2, 1, 0) -= s(1, 0);
  d2(2, 2, 1, 1) += s(0, 0);

  return d2;
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::ddet() const
{
  RankTwoTensorTempl<T> d;

  d(0, 0) = (*this)(1, 1) * (*this)(2, 2) - (*this)(2, 1) * (*this)(1, 2);
  d(0, 1) = (*this)(2, 0) * (*this)(1, 2) - (*this)(1, 0) * (*this)(2, 2);
  d(0, 2) = (*this)(1, 0) * (*this)(2, 1) - (*this)(2, 0) * (*this)(1, 1);
  d(1, 0) = (*this)(2, 1) * (*this)(0, 2) - (*this)(0, 1) * (*this)(2, 2);
  d(1, 1) = (*this)(0, 0) * (*this)(2, 2) - (*this)(2, 0) * (*this)(0, 2);
  d(1, 2) = (*this)(2, 0) * (*this)(0, 1) - (*this)(0, 0) * (*this)(2, 1);
  d(2, 0) = (*this)(0, 1) * (*this)(1, 2) - (*this)(1, 1) * (*this)(0, 2);
  d(2, 1) = (*this)(1, 0) * (*this)(0, 2) - (*this)(0, 0) * (*this)(1, 2);
  d(2, 2) = (*this)(0, 0) * (*this)(1, 1) - (*this)(1, 0) * (*this)(0, 1);

  return d;
}

template <typename T>
void
RankTwoTensorTempl<T>::print(std::ostream & stm) const
{
  const RankTwoTensorTempl<T> & a = *this;
  for (auto i : make_range(N))
  {
    for (auto j : make_range(N))
      stm << std::setw(15) << a(i, j) << ' ';
    stm << std::endl;
  }
}

template <>
void
RankTwoTensor::printReal(std::ostream & stm) const
{
  this->print(stm);
}

template <>
void
ADRankTwoTensor::printReal(std::ostream & stm) const
{
  const ADRankTwoTensor & a = *this;
  for (auto i : make_range(N))
  {
    for (auto j : make_range(N))
      stm << std::setw(15) << a(i, j).value() << ' ';
    stm << std::endl;
  }
}

template <>
void
ADRankTwoTensor::printDualReal(unsigned int nDual, std::ostream & stm) const
{
  const ADRankTwoTensor & a = *this;
  for (auto i : make_range(N))
  {
    for (auto j : make_range(N))
    {
      stm << std::setw(15) << a(i, j).value() << " {";
      for (auto k : make_range(nDual))
        stm << std::setw(5) << a(i, j).derivatives()[k] << ' ';
      stm << " }";
    }
    stm << std::endl;
  }
}

template <typename T>
void
RankTwoTensorTempl<T>::addIa(const T & a)
{
  for (auto i : make_range(N))
    (*this)(i, i) += a;
}

template <typename T>
T
RankTwoTensorTempl<T>::L2norm() const
{
  T norm = 0.0;
  for (auto i : make_range(N2))
  {
    T v = this->_coords[i];
    norm += v * v;
  }
  return norm == 0.0 ? 0.0 : std::sqrt(norm);
}

template <typename T>
void
RankTwoTensorTempl<T>::surfaceFillFromInputVector(const std::vector<T> & input)
{
  if (input.size() == 4)
  {
    // initialize with zeros
    this->zero();
    (*this)(0, 0) = input[0];
    (*this)(0, 1) = input[1];
    (*this)(1, 0) = input[2];
    (*this)(1, 1) = input[3];
  }
  else
    mooseError("please provide correct number of values for surface RankTwoTensorTempl<T> "
               "initialization.");
}

template <typename T>
void
RankTwoTensorTempl<T>::syev(const char *, std::vector<T> &, std::vector<T> &) const
{
  mooseError("The syev method is only supported for Real valued tensors");
}

template <>
void
RankTwoTensor::syev(const char * calculation_type,
                    std::vector<Real> & eigvals,
                    std::vector<Real> & a) const
{
  eigvals.resize(N);
  a.resize(N * N);

  // prepare data for the LAPACKsyev_ routine (which comes from petscblaslapack.h)
  PetscBLASInt nd = N;
  PetscBLASInt lwork = 66 * nd;
  PetscBLASInt info;
  std::vector<PetscScalar> work(lwork);

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      // a is destroyed by dsyev, and if calculation_type == "V" then eigenvectors are placed
      // there Note the explicit symmeterisation
      a[i * N + j] = 0.5 * (this->operator()(i, j) + this->operator()(j, i));

  // compute the eigenvalues only (if calculation_type == "N"),
  // or both the eigenvalues and eigenvectors (if calculation_type == "V")
  // assume upper triangle of a is stored (second "U")
  LAPACKsyev_(calculation_type, "U", &nd, &a[0], &nd, &eigvals[0], &work[0], &lwork, &info);

  if (info != 0)
    mooseError("In computing the eigenvalues and eigenvectors for the symmetric rank-2 tensor (",
               Moose::stringify(a),
               "), the PETSC LAPACK syev routine returned error code ",
               info);
}

template <typename T>
void
RankTwoTensorTempl<T>::symmetricEigenvalues(std::vector<T> & eigvals) const
{
  RankTwoTensorTempl<T> a;
  symmetricEigenvaluesEigenvectors(eigvals, a);
}

template <>
void
RankTwoTensor::symmetricEigenvalues(std::vector<Real> & eigvals) const
{
  std::vector<Real> a;
  syev("N", eigvals, a);
}

template <typename T>
void
RankTwoTensorTempl<T>::symmetricEigenvaluesEigenvectors(std::vector<T> &,
                                                        RankTwoTensorTempl<T> &) const
{
  mooseError(
      "symmetricEigenvaluesEigenvectors is only available for ordered tensor component types");
}

template <>
void
RankTwoTensor::symmetricEigenvaluesEigenvectors(std::vector<Real> & eigvals,
                                                RankTwoTensor & eigvecs) const
{
  std::vector<Real> a;
  syev("V", eigvals, a);

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      eigvecs(j, i) = a[i * N + j];
}

template <>
void
ADRankTwoTensor::symmetricEigenvaluesEigenvectors(std::vector<ADReal> & eigvals,
                                                  ADRankTwoTensor & eigvecs) const
{
  typedef Eigen::Matrix<ADReal, N, N> RankTwoMatrix;
  RankTwoMatrix self;
  for (auto i : make_range(N))
    for (unsigned int j = i; j < N; ++j)
    {
      auto & v = self(j, i);
      v = (*this)(i, j);
      if (i != j && MooseUtils::absoluteFuzzyEqual(v, 0.0))
        v.value() = 0.0;
    }

  Eigen::SelfAdjointEigenSolver<RankTwoMatrix> es;
  es.compute(self);

  const auto & lambda = es.eigenvalues();
  eigvals.resize(N);
  for (auto i : make_range(N))
    eigvals[i] = lambda(i);

  const auto & v = es.eigenvectors();
  for (auto i : make_range(N))
    for (auto j : make_range(N))
      eigvecs(i, j) = v(i, j);
}

template <typename T>
void
RankTwoTensorTempl<T>::dsymmetricEigenvalues(std::vector<T> & eigvals,
                                             std::vector<RankTwoTensorTempl<T>> & deigvals) const
{
  deigvals.resize(N);

  std::vector<T> a;
  syev("V", eigvals, a);

  // now a contains the eigenvetors
  // extract these and place appropriately in deigvals
  std::vector<T> eig_vec;
  eig_vec.resize(N);

  for (auto i : make_range(N))
  {
    for (auto j : make_range(N))
      eig_vec[j] = a[i * N + j];
    for (auto j : make_range(N))
      for (auto k : make_range(N))
        deigvals[i](j, k) = eig_vec[j] * eig_vec[k];
  }

  // There are discontinuities in the derivative
  // for equal eigenvalues.  The following is
  // an attempt to make a sensible choice for
  // the derivative.  This agrees with a central-difference
  // approximation to the derivative.
  if (eigvals[0] == eigvals[1] && eigvals[0] == eigvals[2])
    deigvals[0] = deigvals[1] = deigvals[2] = (deigvals[0] + deigvals[1] + deigvals[2]) / 3.0;
  else if (eigvals[0] == eigvals[1])
    deigvals[0] = deigvals[1] = (deigvals[0] + deigvals[1]) / 2.0;
  else if (eigvals[0] == eigvals[2])
    deigvals[0] = deigvals[2] = (deigvals[0] + deigvals[2]) / 2.0;
  else if (eigvals[1] == eigvals[2])
    deigvals[1] = deigvals[2] = (deigvals[1] + deigvals[2]) / 2.0;
}

template <typename T>
void
RankTwoTensorTempl<T>::d2symmetricEigenvalues(std::vector<RankFourTensorTempl<T>> & deriv) const
{
  std::vector<T> eigvec;
  std::vector<T> eigvals;
  T ev[N][N];

  // reset rank four tensor
  deriv.assign(N, RankFourTensorTempl<T>());

  // get eigen values and eigen vectors
  syev("V", eigvals, eigvec);

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      ev[i][j] = eigvec[i * N + j];

  for (unsigned int alpha = 0; alpha < N; ++alpha)
    for (unsigned int beta = 0; beta < N; ++beta)
    {
      if (eigvals[alpha] == eigvals[beta])
        continue;

      for (auto i : make_range(N))
        for (auto j : make_range(N))
          for (auto k : make_range(N))
            for (auto l : make_range(N))
            {
              deriv[alpha](i, j, k, l) +=
                  0.5 * (ev[beta][i] * ev[alpha][j] + ev[alpha][i] * ev[beta][j]) *
                  (ev[beta][k] * ev[alpha][l] + ev[beta][l] * ev[alpha][k]) /
                  (eigvals[alpha] - eigvals[beta]);
            }
    }
}

template <typename T>
void
RankTwoTensorTempl<T>::getRUDecompositionRotation(RankTwoTensorTempl<T> &) const
{
  mooseError("getRUDecompositionRotation is only supported for Real valued tensors");
}

template <>
void
RankTwoTensor::getRUDecompositionRotation(RankTwoTensor & rot) const
{
  const RankTwoTensor & a = *this;
  RankTwoTensor c, diag, evec;
  PetscScalar cmat[N][N], work[10];
  PetscReal w[N];

  // prepare data for the LAPACKsyev_ routine (which comes from petscblaslapack.h)
  PetscBLASInt nd = N, lwork = 10, info;

  c = a.transpose() * a;

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      cmat[i][j] = c(i, j);

  LAPACKsyev_("V", "U", &nd, &cmat[0][0], &nd, w, work, &lwork, &info);

  if (info != 0)
    mooseError("In computing the eigenvalues and eigenvectors of a symmetric rank-2 tensor, the "
               "PETSC LAPACK syev routine returned error code ",
               info);

  diag.zero();

  for (auto i : make_range(N))
    diag(i, i) = std::sqrt(w[i]);

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      evec(i, j) = cmat[i][j];

  rot = a * (evec.transpose() * diag * evec).inverse();
}

template <typename T>
void
RankTwoTensorTempl<T>::initRandom(unsigned int rand_seed)
{
  MooseRandom::seed(rand_seed);
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::genRandomTensor(T scale, T offset)
{
  RankTwoTensorTempl<T> tensor;

  for (auto i : make_range(N))
    for (auto j : make_range(N))
      tensor(i, j) = (MooseRandom::rand() + offset) * scale;

  return tensor;
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::genRandomSymmTensor(T scale, T offset)
{
  RankTwoTensorTempl<T> tensor;

  for (auto i : make_range(N))
    for (unsigned int j = i; j < N; j++)
      tensor(i, j) = tensor(j, i) = (MooseRandom::rand() + offset) * scale;

  return tensor;
}

template <typename T>
void
RankTwoTensorTempl<T>::vectorOuterProduct(const TypeVector<T> & v1, const TypeVector<T> & v2)
{
  for (auto i : make_range(N))
    for (auto j : make_range(N))
      (*this)(i, j) = v1(i) * v2(j);
}

template <typename T>
void
RankTwoTensorTempl<T>::fillRealTensor(TensorValue<T> & tensor)
{
  for (auto i : make_range(N))
    for (auto j : make_range(N))
      tensor(i, j) = (*this)(i, j);
}

template <typename T>
void
RankTwoTensorTempl<T>::fillRow(unsigned int r, const TypeVector<T> & v)
{
  for (auto i : make_range(N))
    (*this)(r, i) = v(i);
}

template <typename T>
void
RankTwoTensorTempl<T>::fillColumn(unsigned int c, const TypeVector<T> & v)
{
  for (auto i : make_range(N))
    (*this)(i, c) = v(i);
}

template <typename T>
RankTwoTensorTempl<T>
RankTwoTensorTempl<T>::initialContraction(const RankFourTensorTempl<T> & b) const
{
  RankTwoTensorTempl<T> result;

  unsigned int index = 0;
  for (auto i : make_range(N))
    for (auto j : make_range(N))
    {
      const T & a = (*this)(i, j);
      for (auto k : make_range(N))
        for (auto l : make_range(N))
          result(k, l) += a * b._vals[index++];
    }

  return result;
}

template <typename T>
void
RankTwoTensorTempl<T>::setToIdentity()
{
  mooseAssert(N2 == 9, "RankTwoTensorTempl is currently only tested for 3 dimensions.");
  for (auto i : make_range(N2))
    this->_coords[i] = identityCoords[i];
}
