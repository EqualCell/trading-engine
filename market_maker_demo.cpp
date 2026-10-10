#include "market_maker_scenarios.h"

#include <fstream>
#include <iostream>
#include <string>

void printScenario(const ScenarioResult& scenario) {
    std::cout << "Total P&L marked to an externally supplied synthetic reference price.\n"
              << "No fees, latency, margin, or funding costs are modeled.\n"
              << "One-unit quotes; account 1 is exclusively strategy-controlled.\n"
              << "Refresh loses FIFO priority; no post-only rule or refresh on fills.\n\n";
    for (const auto& event : scenario.events) {
        std::cout << "Time (us): " << event.timestamp_us << " | " << event.description << '\n';
        for (const Trade& trade : event.trades) {
            std::cout << "TRADE buy=" << trade.buy_id << " sell=" << trade.sell_id
                      << " price=" << trade.price << " qty=" << trade.quantity << '\n';
        }
        std::cout << "Maker cash_paise=" << event.maker_account.cash_paise
                  << " inventory=" << event.maker_account.inventory
                  << " reference_paise=" << event.reference_price
                  << " portfolio_value_paise=" << event.valuation.portfolio_value_paise
                  << " total_pnl_paise=" << event.valuation.total_pnl_paise << '\n'
                  << "Maker bid=" << (event.maker_bid ? std::to_string(event.maker_bid->price) : "N/A")
                  << " ask=" << (event.maker_ask ? std::to_string(event.maker_ask->price) : "N/A") << "\n\n";
    }
}

int main(int argc, char* argv[]) {
    try {
        std::string name = "inventory-risk";
        std::string output;
        if (argc != 1) {
            if ((argc != 3 && argc != 5) || std::string(argv[1]) != "--scenario" ||
                !knownScenario(argv[2]) ||
                (argc == 5 && (std::string(argv[3]) != "--output" || argv[4][0] == '\0'))) {
                std::cerr << "Usage: " << argv[0]
                          << " [--scenario spread-capture|inventory-risk|inventory-limit [--output filename]]\n";
                return 1;
            }
            name = argv[2];
            if (argc == 5) output = argv[4];
        }
        const auto scenario = runScenario(name);
        if (output.empty()) {
            printScenario(scenario);
        } else {
            std::ofstream file(output, std::ios::binary);
            if (!file) throw std::runtime_error("Cannot open scenario output: " + output);
            writeScenarioJson(file, scenario);
            if (!file) throw std::runtime_error("Error writing scenario output: " + output);
            file.flush();
            if (!file) throw std::runtime_error("Error flushing scenario output: " + output);
            file.close();
            if (!file) throw std::runtime_error("Error closing scenario output: " + output);
            std::cout << "Exported " << name << " to " << output << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        // Refresh and allocation failures may follow partial engine mutation.
        std::cerr << "Fatal demo error: " << error.what() << '\n';
        return 1;
    }
}
