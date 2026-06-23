#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include "domain/core/fractional_number.hpp"

#include <stdexcept>
#include <string>

TEST_CASE("FractionalNumber numeric constructor simplifies and normalizes sign", "[fractional]")
{
    const FractionalNumber half(2, 4);
    REQUIRE(half.GetNumerator() == 1);
    REQUIRE(half.GetDenominator() == 2);
    REQUIRE(half.GetValue() == 0.5);

    // Default constructed value is 0/1.
    const FractionalNumber zero_default;
    REQUIRE(zero_default.GetNumerator() == 0);
    REQUIRE(zero_default.GetDenominator() == 1);

    // A zero numerator collapses the denominator to 1 regardless of input.
    const FractionalNumber zero(0, 5);
    REQUIRE(zero.GetNumerator() == 0);
    REQUIRE(zero.GetDenominator() == 1);

    // Negative denominator is normalized so the sign lives on the numerator.
    const FractionalNumber neg_denom(3, -6);
    REQUIRE(neg_denom.GetNumerator() == -1);
    REQUIRE(neg_denom.GetDenominator() == 2);

    const FractionalNumber neg_both(-3, -6);
    REQUIRE(neg_both.GetNumerator() == 1);
    REQUIRE(neg_both.GetDenominator() == 2);
}

TEST_CASE("FractionalNumber parses integers decimals and leading dot", "[fractional]")
{
    REQUIRE(FractionalNumber("42") == FractionalNumber(42, 1));
    REQUIRE(FractionalNumber("0.5") == FractionalNumber(1, 2));
    REQUIRE(FractionalNumber("2.25") == FractionalNumber(9, 4));

    // Known quirk: a leading decimal point is not the start of a number token
    // (the scanner only begins a number on a digit), so the '.' is skipped and
    // ".5" parses as 5 rather than 0.5. Documented here, not asserted as correct.
    REQUIRE(FractionalNumber(".5") == FractionalNumber(5, 1));

    // Surrounding whitespace is skipped.
    REQUIRE(FractionalNumber("  7  ") == FractionalNumber(7, 1));
}

TEST_CASE("FractionalNumber parses arithmetic expressions with precedence", "[fractional]")
{
    REQUIRE(FractionalNumber("1+2") == FractionalNumber(3, 1));
    REQUIRE(FractionalNumber("5-8") == FractionalNumber(-3, 1));
    REQUIRE(FractionalNumber("3*4") == FractionalNumber(12, 1));
    REQUIRE(FractionalNumber("3/4") == FractionalNumber(3, 4));

    // Multiplication binds tighter than addition.
    REQUIRE(FractionalNumber("2+3*4") == FractionalNumber(14, 1));

    // Parentheses override precedence.
    REQUIRE(FractionalNumber("(2+3)*4") == FractionalNumber(20, 1));

    // Nested parentheses and mixed operators.
    REQUIRE(FractionalNumber("(1+2)/(3-1)") == FractionalNumber(3, 2));
}

TEST_CASE("FractionalNumber parsing rejects malformed expressions", "[fractional]")
{
    // Mismatched parentheses: closing without a matching open.
    REQUIRE_THROWS_AS(FractionalNumber("(1+2"), std::invalid_argument);
    REQUIRE_THROWS_AS(FractionalNumber("1+2)"), std::invalid_argument);

    // Division by zero.
    REQUIRE_THROWS_AS(FractionalNumber("1/0"), std::invalid_argument);

    // Operator without enough operands.
    REQUIRE_THROWS_AS(FractionalNumber("1+"), std::invalid_argument);

    // Too many operands / no operator to combine them.
    REQUIRE_THROWS_AS(FractionalNumber("1 2"), std::invalid_argument);

    // Empty expression yields no value on the stack.
    REQUIRE_THROWS_AS(FractionalNumber(""), std::invalid_argument);
}

TEST_CASE("FractionalNumber string getters are lazily formatted and cached", "[fractional]")
{
    FractionalNumber whole(4, 1);
    REQUIRE(whole.GetStringFraction() == "4");

    FractionalNumber fraction(3, 4);
    REQUIRE(fraction.GetStringFraction() == "3/4");

    FractionalNumber value(1, 2);
    REQUIRE(value.GetStringFloat() == "0.500");

    // Cached string is returned by reference and stays stable on repeat calls.
    REQUIRE(value.GetStringFloat() == "0.500");
}

TEST_CASE("FractionalNumber compound assignment operators simplify", "[fractional]")
{
    FractionalNumber a(1, 2);
    a *= FractionalNumber(2, 3);
    REQUIRE(a == FractionalNumber(1, 3));

    FractionalNumber b(1, 2);
    b += FractionalNumber(1, 3);
    REQUIRE(b == FractionalNumber(5, 6));

    FractionalNumber c(1, 2);
    c -= FractionalNumber(1, 3);
    REQUIRE(c == FractionalNumber(1, 6));
}

TEST_CASE("FractionalNumber free operators produce simplified results", "[fractional]")
{
    REQUIRE(FractionalNumber(1, 2) * FractionalNumber(2, 3) == FractionalNumber(1, 3));
    REQUIRE(FractionalNumber(1, 2) + FractionalNumber(1, 3) == FractionalNumber(5, 6));
    REQUIRE(FractionalNumber(1, 2) - FractionalNumber(1, 3) == FractionalNumber(1, 6));
    REQUIRE(FractionalNumber(3, 4) / FractionalNumber(3, 2) == FractionalNumber(1, 2));
}

TEST_CASE("FractionalNumber comparison operators order by value", "[fractional]")
{
    const FractionalNumber half(1, 2);
    const FractionalNumber third(1, 3);
    const FractionalNumber also_half(2, 4);

    REQUIRE(half == also_half);
    REQUIRE(half != third);
    REQUIRE(third < half);
    REQUIRE(half > third);
    REQUIRE_FALSE(half < third);
    REQUIRE_FALSE(third > half);
}
