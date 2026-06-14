#include <catch2/catch_test_macros.hpp>

#include "domain/linear_solve.hpp"
#include "domain/fractional_number.hpp"

#include <vector>

using Coeffs = std::vector<std::vector<FractionalNumber>>;
using Consts = std::vector<FractionalNumber>;

/// @test   A solvable 2×2 system (x+y=3, x−y=1) row-reduces to the expected upper-triangular echelon
///         form: row 0 unchanged and row 1 eliminated to [0, −2 | −2], encoding the solution x=2, y=1.
/// @covers ReduceMatrix forward elimination on a well-conditioned system — the augmented-matrix
///         layout and the exact pivot/elimination arithmetic underpinning the rate solver.
TEST_CASE("ReduceMatrix row-reduces a 2x2 system exactly", "[linear_solve]")
{
    // x + y = 3
    // x - y = 1   (solution x = 2, y = 1)
    Coeffs a{
        { FractionalNumber(1, 1), FractionalNumber(1, 1) },
        { FractionalNumber(1, 1), FractionalNumber(-1, 1) },
    };
    Consts b{ FractionalNumber(3, 1), FractionalNumber(1, 1) };

    const auto r = ReduceMatrix(a, b, 2);

    // Upper-triangular echelon form: row0 unchanged, row1 eliminated to [0, -2 | -2].
    REQUIRE(r.size() == 2);
    REQUIRE(r[0][0] == FractionalNumber(1, 1));
    REQUIRE(r[0][1] == FractionalNumber(1, 1));
    REQUIRE(r[0][2] == FractionalNumber(3, 1));
    REQUIRE(r[1][0] == FractionalNumber(0, 1));
    REQUIRE(r[1][1] == FractionalNumber(-2, 1));
    REQUIRE(r[1][2] == FractionalNumber(-2, 1)); // => y = 1, then x = 2
}

/// @test   Solving 3x = 1 keeps the result as the exact rational 1/3 (pivot 3, constant 1) instead
///         of a lossy 0.333… float.
/// @covers ReduceMatrix operating over FractionalNumber — the no-floating-point-drift guarantee
///         that lets production rates stay exact through Gaussian elimination.
TEST_CASE("ReduceMatrix keeps exact rationals (no float drift)", "[linear_solve]")
{
    // 3x = 1  => x = 1/3 exactly.
    Coeffs a{ { FractionalNumber(3, 1) } };
    Consts b{ FractionalNumber(1, 1) };

    const auto r = ReduceMatrix(a, b, 1);
    REQUIRE(r.size() == 1);
    REQUIRE(r[0][0] == FractionalNumber(3, 1));
    REQUIRE(r[0][1] == FractionalNumber(1, 1)); // solution = constant/pivot = 1/3
}

/// @test   When the first row has a zero in column 0 (0y+1y=2, x+y=5), elimination swaps the larger
///         pivot row to the top so row 0 leads with coefficient 1.
/// @covers ReduceMatrix partial-pivoting / row-swap path — avoiding a divide-by-zero pivot and
///         keeping the reduction numerically valid when the natural pivot is zero.
TEST_CASE("ReduceMatrix uses partial pivoting (zero leading pivot)", "[linear_solve]")
{
    // First row has a zero in column 0, forcing a row swap.
    // 0x + 1y = 2
    // 1x + 1y = 5   (solution x = 3, y = 2)
    Coeffs a{
        { FractionalNumber(0, 1), FractionalNumber(1, 1) },
        { FractionalNumber(1, 1), FractionalNumber(1, 1) },
    };
    Consts b{ FractionalNumber(2, 1), FractionalNumber(5, 1) };

    const auto r = ReduceMatrix(a, b, 2);
    // Row with the larger |pivot| (the second) is swapped to the top.
    REQUIRE(r[0][0] == FractionalNumber(1, 1));
}

/// @test   Malformed inputs are rejected by throwing: a zero variable count, a constants vector whose
///         length doesn't match the equations, and an equation row whose width doesn't match the
///         variable count.
/// @covers ReduceMatrix precondition/shape validation — defensive guards that fail loudly on caller
///         mistakes instead of reading out of bounds or returning garbage.
TEST_CASE("ReduceMatrix validates shapes", "[linear_solve]")
{
    REQUIRE_THROWS(ReduceMatrix(Coeffs{ { FractionalNumber(1, 1) } }, Consts{ FractionalNumber(1, 1) }, 0));
    // constant count mismatch
    REQUIRE_THROWS(ReduceMatrix(Coeffs{ { FractionalNumber(1, 1) } }, Consts{}, 1));
    // wrong equation width
    REQUIRE_THROWS(ReduceMatrix(Coeffs{ { FractionalNumber(1, 1), FractionalNumber(1, 1) } },
                                Consts{ FractionalNumber(1, 1) }, 1));
}
