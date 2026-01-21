#pragma once
/// @file math_utils.hpp
/// @brief Mathematical utility functions and classes

namespace sample {
namespace math {

/// Basic arithmetic operations
class Calculator {
public:
    Calculator() = default;
    virtual ~Calculator() = default;

    /// Add two integers
    virtual int add(int a, int b) const;

    /// Subtract two integers
    virtual int subtract(int a, int b) const;

    /// Multiply two integers
    virtual int multiply(int a, int b) const;

    /// Divide two integers (returns 0 if b is 0)
    virtual int divide(int a, int b) const;
};

/// Extended calculator with advanced operations
class ScientificCalculator : public Calculator {
public:
    /// Calculate power
    int power(int base, int exp) const;

    /// Calculate factorial
    int factorial(int n) const;

    /// Check if number is prime
    bool is_prime(int n) const;
};

/// Standalone utility functions
int gcd(int a, int b);
int lcm(int a, int b);

/// Template function for absolute value
template<typename T>
T abs_value(T x) {
    return x < 0 ? -x : x;
}

} // namespace math
} // namespace sample
