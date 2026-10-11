#include "execution/comparison.h"
#include <iostream>
#include <set>
ll nonnegative(const std::string& text) {
    execution::require(!text.empty(), "Missing integer");
    ll result = 0;
    for (const char c : text) { execution::require(c >= '0' && c <= '9', "Expected nonnegative integer"); result = execution::add(execution::product(result, 10), c - '0'); }
    return result;
}
int main(int argc, char* argv[]) {
    try {
        ll slices = 3; bool traces = false; std::string scenario = "all";
        execution::Time request = 2, response = 5; std::set<std::string> seen;
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--help" && argc == 2) { std::cout << "execution_compare [--slices N] [--scenario all|rising|falling|insufficient] [--request-delay-us N] [--response-delay-us N] [--trace]\n"; return 0; }
            execution::require(seen.insert(flag).second, "Duplicate option");
            if (flag == "--trace") { traces = true; continue; }
            execution::require((flag == "--slices" || flag == "--scenario" || flag == "--request-delay-us" || flag == "--response-delay-us") && i + 1 < argc, "Unknown or incomplete option; see --help");
            const std::string value = argv[++i];
            if (flag == "--scenario") scenario = value;
            else if (flag == "--slices") slices = nonnegative(value);
            else if (flag == "--request-delay-us") request = nonnegative(value);
            else response = nonnegative(value);
        }
        // Build the complete result before printing, so invalid configuration
        // cannot leave a misleading partial comparison on stdout.
        const std::string result = execution::comparisonText(slices, traces, scenario, request, response);
        std::cout << result;
    } catch (const std::exception& error) { std::cerr << "Execution comparison failed: " << error.what() << '\n'; return 1; }
}
