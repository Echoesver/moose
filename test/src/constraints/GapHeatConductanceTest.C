//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#include "GapHeatConductanceTest.h"
#include "Function.h"
#include "Assembly.h"

registerMooseObject("MooseTestApp", GapHeatConductanceTest);

InputParameters
GapHeatConductanceTest::validParams()
{
  InputParameters params = ADMortarConstraint::validParams();
  params.addParam<MaterialPropertyName>(
      "secondary_gap_conductance",
      "gap_conductance",
      "The material property name providing the gap conductance on the secondary side");
  params.addParam<MaterialPropertyName>(
      "primary_gap_conductance",
      "gap_conductance",
      "The material property name providing the gap conductance on the primary side");
  params.addParam<FunctionName>(
      "secondary_mms_function", 0, "An mms function to apply to the secondary side");
  params.addParam<FunctionName>(
      "primary_mms_function", 0, "An mms function to apply to the primary side");
  return params;
}

GapHeatConductanceTest::GapHeatConductanceTest(const InputParameters & parameters)
  : ADMortarConstraint(parameters),
    _secondary_gap_conductance(getADMaterialProperty<Real>("secondary_gap_conductance")),
    _primary_gap_conductance(getNeighborADMaterialProperty<Real>("primary_gap_conductance")),
    _secondary_mms_function(getFunction<Real>("secondary_mms_function")),
    _primary_mms_function(getFunctionByName<Real>(getParam<FunctionName>("primary_mms_function")))
{
}

ADReal
GapHeatConductanceTest::computeQpResidual(Moose::MortarType type)
{
  switch (type)
  {
    case Moose::MortarType::Secondary:
      return (_lambda[_qp] + _secondary_mms_function.value(_t, _phys_points_secondary[_qp])) *
             _test_secondary[_i][_qp];

    case Moose::MortarType::Primary:
      return (-_lambda[_qp] + _primary_mms_function.value(_t, _phys_points_primary[_qp])) *
             _test_primary[_i][_qp];

    case Moose::MortarType::Lower:
    {
      ADReal heat_transfer_coeff(0);
      auto gap = (_phys_points_secondary[_qp] - _phys_points_primary[_qp]).norm();
      mooseAssert(MetaPhysicL::raw_value(gap) > TOLERANCE * TOLERANCE,
                  "Gap distance is too small in GapHeatConductanceTest");

      heat_transfer_coeff =
          (0.5 * (_secondary_gap_conductance[_qp] + _primary_gap_conductance[_qp])) / gap;

      return _test[_i][_qp] *
             (_lambda[_qp] - heat_transfer_coeff * (_u_secondary[_qp] - _u_primary[_qp]));
    }

    default:
      return 0;
  }
}

void
GapHeatConductanceTest::computeJacobian(Moose::MortarType mortar_type)
{
  std::vector<DualReal> residuals;
  size_t test_space_size = 0;
  typedef Moose::ConstraintJacobianType JType;
  typedef Moose::MortarType MType;
  std::vector<JType> jacobian_types;
  std::vector<dof_id_type> dof_indices;

  switch (mortar_type)
  {
    case MType::Secondary:
      dof_indices = _secondary_var.dofIndices();
      jacobian_types = {JType::SecondarySecondary, JType::SecondaryPrimary, JType::SecondaryLower};
      break;

    case MType::Primary:
      dof_indices = _primary_var.dofIndicesNeighbor();
      jacobian_types = {JType::PrimarySecondary, JType::PrimaryPrimary, JType::PrimaryLower};
      break;

    case MType::Lower:
      if (_var)
        dof_indices = _var->dofIndicesLower();
      jacobian_types = {JType::LowerSecondary, JType::LowerPrimary, JType::LowerLower};
      break;
  }
  test_space_size = dof_indices.size();

  residuals.resize(test_space_size, 0);
  for (_qp = 0; _qp < _qrule_msm->n_points(); _qp++)
    for (_i = 0; _i < test_space_size; _i++)
      residuals[_i] += _JxW_msm[_qp] * _coord[_qp] * computeQpResidual(mortar_type);

#ifdef MOOSE_GLOBAL_AD_INDEXING
  // Trim interior node variable derivatives
  const auto & primary_ip_lowerd_map = amg().getPrimaryIpToLowerElementMap(
      *_lower_primary_elem, *_lower_primary_elem->interior_parent(), *_lower_secondary_elem);
  const auto & secondary_ip_lowerd_map =
      amg().getSecondaryIpToLowerElementMap(*_lower_secondary_elem);
  const std::vector<const MooseVariable *> var_array = {&_secondary_var, &_primary_var};

  trimInteriorNodeDerivatives(secondary_ip_lowerd_map, var_array, residuals, true);
  trimInteriorNodeDerivatives(primary_ip_lowerd_map, var_array, residuals, false);
  _assembly.processUnconstrainedDerivatives(residuals, dof_indices, _matrix_tags);

#else

  auto local_functor = [&](const std::vector<ADReal> & input_residuals,
                           const std::vector<dof_id_type> &,
                           const std::set<TagID> &)
  {
    auto & ce = _assembly.couplingEntries();
    for (const auto & it : ce)
    {
      MooseVariableFEBase & ivariable = *(it.first);
      MooseVariableFEBase & jvariable = *(it.second);

      unsigned int ivar = ivariable.number();
      unsigned int jvar = jvariable.number();

      switch (mortar_type)
      {
        case MType::Secondary:
          if (ivar != _secondary_var.number())
            continue;
          break;

        case MType::Primary:
          if (ivar != _primary_var.number())
            continue;
          break;

        case MType::Lower:
          if (!_var || _var->number() != ivar)
            continue;
          break;
      }

      // Derivatives are offset by the variable number
      std::vector<size_t> ad_offsets{
          Moose::adOffset(jvar, _sys.getMaxVarNDofsPerElem(), Moose::ElementType::Element),
          Moose::adOffset(jvar,
                          _sys.getMaxVarNDofsPerElem(),
                          Moose::ElementType::Neighbor,
                          _sys.system().n_vars()),
          Moose::adOffset(jvar,
                          _sys.getMaxVarNDofsPerElem(),
                          Moose::ElementType::Lower,
                          _sys.system().n_vars())};
      std::vector<size_t> shape_space_sizes{jvariable.dofIndices().size(),
                                            jvariable.dofIndicesNeighbor().size(),
                                            jvariable.dofIndicesLower().size()};

      for (MooseIndex(3) type_index = 0; type_index < 3; ++type_index)
      {
        const auto jacobian_type = jacobian_types[type_index];
        // There's no actual coupling between secondary and primary dofs
        if ((jacobian_type == JType::SecondaryPrimary) ||
            (jacobian_type == JType::PrimarySecondary))
          continue;

        prepareMatrixTagLower(_assembly, ivar, jvar, jacobian_type);
        for (_i = 0; _i < test_space_size; _i++)
          for (_j = 0; _j < shape_space_sizes[type_index]; _j++)
          {
#ifndef MOOSE_SPARSE_AD
            mooseAssert(ad_offsets[type_index] + _j < MOOSE_AD_MAX_DOFS_PER_ELEM,
                        "Out of bounds access in derivative vector.");
#endif
            _local_ke(_i, _j) += input_residuals[_i].derivatives()[ad_offsets[type_index] + _j];
          }
        accumulateTaggedLocalMatrix();
      }
    }
  };

  _assembly.processDerivatives(residuals, dof_indices, _matrix_tags, local_functor);
#endif
}
