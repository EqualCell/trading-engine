#include "market_maker.h"

#include <iostream>
#include <string>

int main() {
    try {
        TradingSimulator simulator;
        constexpr ull maker_trader = 1;
        constexpr ull external_trader = 2;
        constexpr ll initial_cash = 100000;
        simulator.createAccount(maker_trader, initial_cash, 0);
        simulator.createAccount(external_trader, 100000, 10);
        OrderIdSource ids;
        MarketMaker maker(simulator, ids, maker_trader, 100, 5);
        ll reference = 10000;

        // The only external submission path fixes trader 2. Trader 1 is reserved
        // exclusively for maker.refreshQuotes; no direct maker orders/cancels.
        auto externalOrder = [&](Side side, ll price) {
            return simulator.submitOrder(external_trader, {ids.next(), side, price, 1});
        };
        auto report = [&](ull timestamp, const std::string& event,
                          const std::vector<Trade>& trades) {
            std::cout << "Time (us): " << timestamp << " | " << event << '\n';
            for (const Trade& trade : trades) {
                std::cout << "TRADE buy=" << trade.buy_id << " sell=" << trade.sell_id
                          << " price=" << trade.price << " qty=" << trade.quantity << '\n';
            }
            // Accounting was already applied by TradingSimulator.
            const Account& account = simulator.getAccount(maker_trader);
            const auto mark = markedValuation(account, reference, initial_cash);
            const auto bid = simulator.orderBook().bestBid();
            const auto ask = simulator.orderBook().bestAsk();
            std::cout << "Maker cash_paise=" << account.cash_paise
                      << " inventory=" << account.inventory
                      << " reference_paise=" << reference
                      << " portfolio_value_paise=" << mark.portfolio_value_paise
                      << " total_pnl_paise=" << mark.total_pnl_paise << '\n'
                      << "Book bid=" << (bid ? std::to_string(*bid) : "N/A")
                      << " ask=" << (ask ? std::to_string(*ask) : "N/A") << "\n\n";
        };
        std::cout << "Total P&L marked to an externally supplied synthetic reference price.\n"
                  << "No fees, latency, margin, or funding costs are modeled.\n"
                  << "One-unit quotes; account 1 is exclusively strategy-controlled.\n"
                  << "Refresh loses FIFO priority; no post-only rule or refresh on fills.\n\n";
        report(0, "Reference 10000: refresh quotes", maker.refreshQuotes(reference));
        report(1000, "External trader sells one at limit 9900", externalOrder(Side::Sell, 9900));
        report(2000, "External trader buys one at limit 10100", externalOrder(Side::Buy, 10100));
        report(3000, "Reference unchanged: refresh quotes", maker.refreshQuotes(reference));
        report(4000, "External trader sells one at limit 9900", externalOrder(Side::Sell, 9900));
        reference = 9500;
        report(5000, "Reference drops to 9500: refresh quotes", maker.refreshQuotes(reference));
        return 0;
    } catch (const std::exception& error) {
        // Strategy refresh is not transactional: terminate on failures, including
        // engine allocation failure or exhaustion after part of a refresh.
        std::cerr << "Fatal demo error: " << error.what() << '\n';
        return 1;
    }
}
