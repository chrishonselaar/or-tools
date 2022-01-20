// Copyright 2010-2021 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ortools/math_opt/validators/model_validator.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "ortools/base/integral_types.h"
#include "ortools/base/status_macros.h"
#include "ortools/math_opt/core/model_summary.h"
#include "ortools/math_opt/core/sparse_vector_view.h"
#include "ortools/math_opt/model.pb.h"
#include "ortools/math_opt/model_update.pb.h"
#include "ortools/math_opt/sparse_containers.pb.h"
#include "ortools/math_opt/validators/ids_validator.h"
#include "ortools/math_opt/validators/name_validator.h"
#include "ortools/math_opt/validators/scalar_validator.h"
#include "ortools/math_opt/validators/sparse_matrix_validator.h"
#include "ortools/math_opt/validators/sparse_vector_validator.h"

namespace operations_research {
namespace math_opt {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

////////////////////////////////////////////////////////////////////////////////
// Submessages
////////////////////////////////////////////////////////////////////////////////

absl::Status VariablesValid(const VariablesProto& variables,
                            const bool check_names) {
  RETURN_IF_ERROR(CheckIdsRangeAndStrictlyIncreasing(variables.ids()))
      << "Bad variable ids";
  RETURN_IF_ERROR(
      CheckValues(MakeView(variables.ids(), variables.lower_bounds()),
                  {.allow_positive_infinity = false}, "lower_bounds"));
  RETURN_IF_ERROR(
      CheckValues(MakeView(variables.ids(), variables.upper_bounds()),
                  {.allow_negative_infinity = false}, "upper_bounds"));
  RETURN_IF_ERROR(
      CheckValues(MakeView(variables.ids(), variables.integers()), "integers"));
  RETURN_IF_ERROR(CheckNameVector(MakeView(variables.ids(), variables.names()),
                                  check_names));
  return absl::OkStatus();
}

absl::Status VariableUpdatesValid(
    const VariableUpdatesProto& variable_updates) {
  RETURN_IF_ERROR(CheckIdsAndValues(MakeView(variable_updates.lower_bounds()),
                                    {.allow_positive_infinity = false}))
      << "Bad lower bounds";
  RETURN_IF_ERROR(CheckIdsAndValues(MakeView(variable_updates.upper_bounds()),
                                    {.allow_negative_infinity = false}))
      << "Bad upper bounds";
  RETURN_IF_ERROR(CheckIdsAndValues(MakeView(variable_updates.integers())))
      << "Bad integers";
  return absl::OkStatus();
}

absl::Status VariableUpdatesValidForState(
    const VariableUpdatesProto& variable_updates,
    const IdUpdateValidator& id_validator) {
  RETURN_IF_ERROR(id_validator.CheckSortedIdsSubsetOfNotDeleted(
      variable_updates.lower_bounds().ids()))
      << "lower bound update on invalid variable id";
  RETURN_IF_ERROR(id_validator.CheckSortedIdsSubsetOfNotDeleted(
      variable_updates.upper_bounds().ids()))
      << "upper bound update on invalid variable id";
  RETURN_IF_ERROR(id_validator.CheckSortedIdsSubsetOfNotDeleted(
      variable_updates.integers().ids()))
      << "integer update on invalid variable id";
  return absl::OkStatus();
}

absl::Status ObjectiveValid(const ObjectiveProto& objective,
                            absl::Span<const int64_t> variable_ids) {
  // 1. Validate offset
  RETURN_IF_ERROR(CheckScalarNoNanNoInf(objective.offset()))
      << "Objective offset invalid";
  // 2. Validate linear terms
  const auto linear_coefficients = MakeView(objective.linear_coefficients());
  RETURN_IF_ERROR(CheckIdsAndValues(
      linear_coefficients,
      {.allow_positive_infinity = false, .allow_negative_infinity = false}))
      << "Linear objective coefficients bad";
  RETURN_IF_ERROR(CheckSortedIdsSubset(linear_coefficients.ids(), variable_ids))
      << "Objective.linear_coefficients.ids not found in Variables.ids";
  // 3. Validate quadratic terms
  RETURN_IF_ERROR(SparseMatrixValid(objective.quadratic_coefficients(),
                                    /*enforce_upper_triangular=*/true))
      << "Objective.quadratic_coefficients invalid";
  RETURN_IF_ERROR(SparseMatrixIdsAreKnown(objective.quadratic_coefficients(),
                                          variable_ids, variable_ids))
      << "Objective.quadratic_coefficients invalid";
  return absl::OkStatus();
}

// NOTE: This method does not check requirements on the IDs
absl::Status ObjectiveUpdatesValid(
    const ObjectiveUpdatesProto& objective_updates) {
  // 1. Validate offset
  RETURN_IF_ERROR(CheckScalarNoNanNoInf(objective_updates.offset_update()))
      << "Offset update invalid";
  // 2. Validate linear terms
  RETURN_IF_ERROR(CheckIdsAndValues(
      MakeView(objective_updates.linear_coefficients()),
      {.allow_positive_infinity = false, .allow_negative_infinity = false}))
      << "Linear objective coefficients bad";
  // 3. Validate quadratic terms
  RETURN_IF_ERROR(SparseMatrixValid(objective_updates.quadratic_coefficients(),
                                    /*enforce_upper_triangular=*/true))
      << "Objective.quadratic_coefficients invalid";
  return absl::OkStatus();
}

absl::Status ObjectiveUpdatesValidForModel(
    const ObjectiveUpdatesProto& objective_updates,
    const IdUpdateValidator& id_validator) {
  RETURN_IF_ERROR(id_validator.CheckSortedIdsSubsetOfFinal(
      objective_updates.linear_coefficients().ids()))
      << "Linear coefficients ids not found in variable ids";
  RETURN_IF_ERROR(id_validator.CheckSortedIdsSubsetOfFinal(
      objective_updates.quadratic_coefficients().row_ids()))
      << "Quadratic coefficient ids bad";
  RETURN_IF_ERROR(id_validator.CheckIdsSubsetOfFinal(
      objective_updates.quadratic_coefficients().column_ids()))
      << "Quadratic coefficient ids bad";
  return absl::OkStatus();
}

absl::Status LinearConstraintsValid(
    const LinearConstraintsProto& linear_constraints, const bool check_names) {
  RETURN_IF_ERROR(CheckIdsRangeAndStrictlyIncreasing(linear_constraints.ids()))
      << "Bad linear constraint ids";
  RETURN_IF_ERROR(CheckValues(
      MakeView(linear_constraints.ids(), linear_constraints.lower_bounds()),
      {.allow_positive_infinity = false}, "lower_bounds"));
  RETURN_IF_ERROR(CheckValues(
      MakeView(linear_constraints.ids(), linear_constraints.upper_bounds()),
      {.allow_negative_infinity = false}, "upper_bounds"));
  RETURN_IF_ERROR(CheckNameVector(
      MakeView(linear_constraints.ids(), linear_constraints.names()),
      check_names));
  return absl::OkStatus();
}

absl::Status LinearConstraintUpdatesValid(
    const LinearConstraintUpdatesProto& linear_constraint_updates) {
  RETURN_IF_ERROR(
      CheckIdsAndValues(MakeView(linear_constraint_updates.lower_bounds()),
                        {.allow_positive_infinity = false}))
      << "Bad lower bounds";
  RETURN_IF_ERROR(
      CheckIdsAndValues(MakeView(linear_constraint_updates.upper_bounds()),
                        {.allow_negative_infinity = false}))
      << "Bad upper bounds";
  return absl::OkStatus();
}

absl::Status LinearConstraintUpdatesValidForState(
    const LinearConstraintUpdatesProto& linear_constraint_updates,
    const IdUpdateValidator& id_validator) {
  RETURN_IF_ERROR(id_validator.CheckSortedIdsSubsetOfNotDeleted(
      linear_constraint_updates.lower_bounds().ids()))
      << "lower bound update on invalid linear constraint id";
  RETURN_IF_ERROR(id_validator.CheckSortedIdsSubsetOfNotDeleted(
      linear_constraint_updates.upper_bounds().ids()))
      << "upper bound update on invalid linear constraint id";
  return absl::OkStatus();
}

absl::Status LinearConstraintMatrixIdsValidForUpdate(
    const SparseDoubleMatrixProto& matrix,
    const IdUpdateValidator& linear_constraint_id_validator,
    const IdUpdateValidator& variable_id_validator) {
  RETURN_IF_ERROR(linear_constraint_id_validator.CheckSortedIdsSubsetOfFinal(
      matrix.row_ids()))
      << "Unknown linear_constraint_id";
  RETURN_IF_ERROR(
      variable_id_validator.CheckIdsSubsetOfFinal(matrix.column_ids()))
      << "Unknown variable_id";
  return absl::OkStatus();
}

}  // namespace

// /////////////////////////////////////////////////////////////////////////////
// Model
// /////////////////////////////////////////////////////////////////////////////

absl::Status ValidateModel(const ModelProto& model, const bool check_names) {
  RETURN_IF_ERROR(VariablesValid(model.variables(), check_names))
      << "Model.variables are invalid.";
  RETURN_IF_ERROR(ObjectiveValid(model.objective(), model.variables().ids()))
      << "Model.objective is invalid";
  RETURN_IF_ERROR(
      LinearConstraintsValid(model.linear_constraints(), check_names))
      << "Model.linear_constraints are invalid";
  RETURN_IF_ERROR(SparseMatrixValid(model.linear_constraint_matrix()))
      << "Model.linear_constraint_matrix invalid";
  RETURN_IF_ERROR(SparseMatrixIdsAreKnown(model.linear_constraint_matrix(),
                                          model.linear_constraints().ids(),
                                          model.variables().ids()))
      << "Model.linear_constraint_matrix ids are inconsistent";
  return absl::OkStatus();
}

////////////////////////////////////////////////////////////////////////////////
// Model Update
////////////////////////////////////////////////////////////////////////////////

absl::Status ValidateModelUpdate(const ModelUpdateProto& model_update,
                                 const bool check_names) {
  RETURN_IF_ERROR(CheckIdsRangeAndStrictlyIncreasing(
      model_update.deleted_linear_constraint_ids()))
      << "ModelUpdateProto.deleted_linear_constraint_ids invalid";
  RETURN_IF_ERROR(
      CheckIdsRangeAndStrictlyIncreasing(model_update.deleted_variable_ids()))
      << "ModelUpdateProto.deleted_variable_ids invalid";
  RETURN_IF_ERROR(VariableUpdatesValid(model_update.variable_updates()))
      << "ModelUpdateProto.variable_updates invalid";
  RETURN_IF_ERROR(
      LinearConstraintUpdatesValid(model_update.linear_constraint_updates()))
      << "ModelUpdateProto.linear_constraint_updates invalid";
  RETURN_IF_ERROR(VariablesValid(model_update.new_variables(), check_names))
      << "ModelUpdateProto.new_variables invalid";
  RETURN_IF_ERROR(LinearConstraintsValid(model_update.new_linear_constraints(),
                                         check_names))
      << "ModelUpdateProto.new_linear_constraints invalid";
  RETURN_IF_ERROR(ObjectiveUpdatesValid(model_update.objective_updates()))
      << "ModelUpdateProto.objective_update invalid";
  RETURN_IF_ERROR(
      SparseMatrixValid(model_update.linear_constraint_matrix_updates()))
      << "Model.linear_constraint_matrix_updates invalid";
  return absl::OkStatus();
}

absl::Status ValidateModelUpdateAndSummary(const ModelUpdateProto& model_update,
                                           const ModelSummary& model_summary,
                                           const bool check_names) {
  RETURN_IF_ERROR(ValidateModelUpdate(model_update));
  const IdUpdateValidator variable_id_validator(
      model_summary.variables, model_update.deleted_variable_ids(),
      model_update.new_variables().ids());
  RETURN_IF_ERROR(variable_id_validator.IsValid())
      << "Invalid new or deleted variable id";
  const IdUpdateValidator linear_constraint_id_validator(
      model_summary.linear_constraints,
      model_update.deleted_linear_constraint_ids(),
      model_update.new_linear_constraints().ids());
  RETURN_IF_ERROR(linear_constraint_id_validator.IsValid())
      << "Invalid new or deleted linear constraint id";

  RETURN_IF_ERROR(VariableUpdatesValidForState(model_update.variable_updates(),
                                               variable_id_validator))
      << "Invalid variable update";

  RETURN_IF_ERROR(LinearConstraintUpdatesValidForState(
      model_update.linear_constraint_updates(), linear_constraint_id_validator))
      << "Invalid linear constraint update";

  RETURN_IF_ERROR(ObjectiveUpdatesValidForModel(
      model_update.objective_updates(), variable_id_validator))
      << "Invalid objective update";
  RETURN_IF_ERROR(LinearConstraintMatrixIdsValidForUpdate(
      model_update.linear_constraint_matrix_updates(),
      linear_constraint_id_validator, variable_id_validator))
      << "Invalid linear constraint matrix update";
  if (check_names && !model_update.new_variables().names().empty()) {
    RETURN_IF_ERROR(
        CheckNewNames(model_summary.variables,
                      MakeView(model_update.new_variables().ids(),
                               model_update.new_variables().names())))
        << "Bad new variable names";
  }

  if (check_names && !model_update.new_linear_constraints().names().empty()) {
    RETURN_IF_ERROR(
        CheckNewNames(model_summary.linear_constraints,
                      MakeView(model_update.new_linear_constraints().ids(),
                               model_update.new_linear_constraints().names())))
        << "Bad new linear constraint names";
  }
  return absl::OkStatus();
}

}  // namespace math_opt
}  // namespace operations_research
