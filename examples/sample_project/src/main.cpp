/// @file main.cpp
/// @brief Sample application demonstrating clangsql analysis

#include <sample/math_utils.hpp>
#include <sample/string_utils.hpp>
#include <iostream>

using namespace sample;

int main(int argc, char* argv[]) {
    std::cout << "Sample Project v" << PROJECT_VERSION << "\n\n";

    // Math utilities demo
    math::Calculator calc;
    std::cout << "Calculator demo:\n";
    std::cout << "  10 + 5 = " << calc.add(10, 5) << "\n";
    std::cout << "  10 - 5 = " << calc.subtract(10, 5) << "\n";
    std::cout << "  10 * 5 = " << calc.multiply(10, 5) << "\n";
    std::cout << "  10 / 5 = " << calc.divide(10, 5) << "\n";

    math::ScientificCalculator sci_calc;
    std::cout << "\nScientific calculator demo:\n";
    std::cout << "  2^10 = " << sci_calc.power(2, 10) << "\n";
    std::cout << "  5! = " << sci_calc.factorial(5) << "\n";
    std::cout << "  is_prime(17) = " << (sci_calc.is_prime(17) ? "true" : "false") << "\n";

    std::cout << "\nStandalone math functions:\n";
    std::cout << "  gcd(48, 18) = " << math::gcd(48, 18) << "\n";
    std::cout << "  lcm(4, 6) = " << math::lcm(4, 6) << "\n";

    // String utilities demo
    strings::StringConfig config;
    config.delimiter = ',';
    config.trim_whitespace = true;

    strings::StringUtils str_utils(config);
    std::cout << "\nString utilities demo:\n";
    std::cout << "  to_upper('hello') = " << str_utils.to_upper("hello") << "\n";
    std::cout << "  reverse('hello') = " << strings::reverse("hello") << "\n";

    auto parts = str_utils.split("one, two, three");
    std::cout << "  split('one, two, three') = [";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << "'" << parts[i] << "'";
    }
    std::cout << "]\n";

    return 0;
}
