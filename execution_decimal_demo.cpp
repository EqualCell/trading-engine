#include "execution/decimal_demo.h"
#include <iostream>
int main(int argc, char* argv[]) {
    try {
        const std::string mode = argc == 1 ? "all" : argv[1];
        execution::require(argc <= 2 && (mode == "all" || mode == "immediate" || mode == "partial" || mode == "scheduled"), "Usage: execution_decimal_demo [immediate|partial|scheduled]");
        if (mode == "all" || mode == "immediate") std::cout << execution::fractionalDemo();
        if (mode == "all" || mode == "partial") std::cout << execution::fractionalDemo(false, true);
        if (mode == "all" || mode == "scheduled") std::cout << execution::fractionalDemo(true);
    } catch (const std::exception& error) { std::cerr << "Decimal execution experiment failed: " << error.what() << '\n'; return 1; }
}
