#include "execution/experiment.h"
#include <iostream>
int main(int argc, char* argv[]) {
    try {
        if (argc > 2 || (argc == 2 && std::string(argv[1]) != "immediate" && std::string(argv[1]) != "race")) {
            std::cerr << "Usage: execution_demo [immediate|race]\n"; return 1;
        }
        if (argc == 1 || std::string(argv[1]) == "immediate") std::cout << "DEMO A: immediate bounded-limit purchase\n" << execution::immediateDemo();
        if (argc == 1 || std::string(argv[1]) == "race") std::cout << "DEMO B: cancellation race\n" << execution::raceDemo();
    } catch (const std::exception& error) { std::cerr << "Execution experiment failed: " << error.what() << '\n'; return 1; }
}
