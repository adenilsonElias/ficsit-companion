#include "domain/solver/linear_solve.hpp"

#include <cmath>
#include <stdexcept>

std::vector<std::vector<FractionalNumber>> ReduceMatrix(
    const std::vector<std::vector<FractionalNumber>>& equations_coefficients,
    const std::vector<FractionalNumber>& constants,
    size_t num_variables)
{
    const size_t num_equations = equations_coefficients.size();
    std::vector<std::vector<FractionalNumber>> reduced_matrix;
    // Check sizes are ok
    if (num_variables == 0)
    {
        throw std::runtime_error("Wrong number of variables");
    }
    if (constants.size() != equations_coefficients.size())
    {
        throw std::runtime_error("Wrong number of constants");
    }
    for (const auto& equation : equations_coefficients)
    {
        if (equation.size() != num_variables)
        {
            throw std::runtime_error("Missing a variable in equation");
        }
    }

    // Create the system matrix [equations_coefficients|constants]
    reduced_matrix = std::vector<std::vector<FractionalNumber>>(num_equations, std::vector<FractionalNumber>(num_variables + 1));
    for (size_t i = 0; i < num_equations; ++i)
    {
        std::copy_n(equations_coefficients[i].begin(), num_variables, reduced_matrix[i].begin());
        reduced_matrix[i][num_variables] = constants[i];
    }

    size_t h = 0;
    size_t k = 0;
    // Gaussian elimination loop
    while (h < num_equations && k < num_variables)
    {
        // Find k-th pivot
        size_t i_max = h;
        // Max double == max fraction
        double v_max = std::abs(reduced_matrix[h][k].GetValue());
        for (size_t i = h + 1; i < num_equations; ++i)
        {
            const double abs_val = std::abs(reduced_matrix[i][k].GetValue());
            if (abs_val > v_max)
            {
                v_max = abs_val;
                i_max = i;
            }
        }

        // If pivot column is zero
        if (reduced_matrix[i_max][k].GetNumerator() == 0)
        {
            k += 1;
            continue;
        }

        // Swap rows if necessary
        if (i_max != h)
        {
            std::swap(reduced_matrix[h], reduced_matrix[i_max]);
        }

        // Eliminate below pivot
        for (size_t i = h + 1; i < num_equations; ++i)
        {
            // We checked that reduced_matrix[h==i_max (swapped)][k] != 0 above
            const FractionalNumber factor = reduced_matrix[i][k] / reduced_matrix[h][k];
            // Set pivot element to 0
            reduced_matrix[i][k] = 0;
            // Update remaining elements in row
            for (size_t j = k + 1; j <= num_variables; ++j)
            {
                reduced_matrix[i][j] -= reduced_matrix[h][j] * factor;
            }
        }
        h += 1;
        k += 1;
    }

    return reduced_matrix;
}
