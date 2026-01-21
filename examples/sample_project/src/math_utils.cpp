/// @file math_utils.cpp
/// @brief Implementation of mathematical utilities

#include <sample/math_utils.hpp>

namespace sample {
namespace math {

int Calculator::add(int a, int b) const {
    return a + b;
}

int Calculator::subtract(int a, int b) const {
    return a - b;
}

int Calculator::multiply(int a, int b) const {
    return a * b;
}

int Calculator::divide(int a, int b) const {
    if (b == 0) return 0;
    return a / b;
}

int ScientificCalculator::power(int base, int exp) const {
    if (exp < 0) return 0;
    int result = 1;
    for (int i = 0; i < exp; ++i) {
        result *= base;
    }
    return result;
}

int ScientificCalculator::factorial(int n) const {
    if (n < 0) return 0;
    if (n <= 1) return 1;
    int result = 1;
    for (int i = 2; i <= n; ++i) {
        result *= i;
    }
    return result;
}

bool ScientificCalculator::is_prime(int n) const {
    if (n <= 1) return false;
    if (n <= 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (int i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) {
            return false;
        }
    }
    return true;
}

int gcd(int a, int b) {
    a = abs_value(a);
    b = abs_value(b);
    while (b != 0) {
        int temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

int lcm(int a, int b) {
    if (a == 0 || b == 0) return 0;
    return abs_value(a * b) / gcd(a, b);
}

} // namespace math
} // namespace sample
