#pragma once

#include "domain/core/fractional_number.hpp"

#include <vector>

/// @brief Gaussian elimination (partial pivoting) to row-echelon form on the
/// augmented system [equations_coefficients | constants], using exact rational
/// arithmetic. Returns the reduced augmented matrix with `num_equations` rows
/// and `num_variables + 1` columns (the last column holding the constants).
///
/// Extracted verbatim from RateSolver::Solve so the numeric kernel can be unit
/// tested in isolation.
///
/// @throws std::runtime_error if `num_variables == 0`, the constant count does
///         not match the equation count, or any equation has the wrong width.
std::vector<std::vector<FractionalNumber>> ReduceMatrix(
    const std::vector<std::vector<FractionalNumber>>& equations_coefficients,
    const std::vector<FractionalNumber>& constants,
    size_t num_variables);
