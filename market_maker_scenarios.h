#pragma once

#include "market_maker.h"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>

struct EventSnapshot {
    ull index;
    ull timestamp_us;
    std::string type;
    std::string description;
    ll reference_price;
    std::optional<Order> maker_bid;
    std::optional<Order> maker_ask;
    Account maker_account;
    MarkedValuation valuation;
    std::vector<Trade> trades;
};

struct ScenarioResult {
    std::string name;
    std::string description;
    std::vector<EventSnapshot> events;
    static constexpr ll initial_cash = 100000;
    static constexpr ll half_spread = 100;
    static constexpr ll inventory_limit = 5;
};

inline bool knownScenario(const std::string& name) {
    return name == "spread-capture" || name == "inventory-risk" || name == "inventory-limit";
}

inline ScenarioResult runScenario(const std::string& name) {
    if (!knownScenario(name)) throw std::invalid_argument("Unknown scenario: " + name);
    ScenarioResult result;
    result.name = name;
    result.description = name == "spread-capture"
        ? "A one-unit round trip captures 200 paise before fees."
        : name == "inventory-risk"
        ? "A profitable round trip is followed by inventory exposure and a reference-price drop."
        : "Repeated independent buy fills reach the long inventory limit; the next refresh suppresses bids.";
    TradingSimulator simulator;
    simulator.createAccount(1, ScenarioResult::initial_cash, 0);
    simulator.createAccount(2, 100000, 10);
    OrderIdSource ids;
    MarketMaker maker(simulator, ids, 1, ScenarioResult::half_spread, ScenarioResult::inventory_limit);
    ll reference = 10000;
    auto quote = [&](std::optional<ull> id) {
        return id ? simulator.orderBook().activeOrder(*id) : std::optional<Order>{};
    };
    auto snapshot = [&](ull time, const std::string& type, const std::string& description,
                        std::vector<Trade> trades) {
        const Account account = simulator.getAccount(1);
        result.events.push_back({static_cast<ull>(result.events.size()), time, type, description,
                                 reference, quote(maker.lastBuyId()), quote(maker.lastSellId()),
                                 account, markedValuation(account, reference, ScenarioResult::initial_cash),
                                 std::move(trades)});
    };
    auto refresh = [&](ull time, ll price, const std::string& description) {
        reference = price;
        snapshot(time, "quote_refresh", description, maker.refreshQuotes(reference));
    };
    // This path always submits for trader 2; maker account 1 is exclusively
    // strategy-controlled. All orders share ids, and all trades reach snapshots.
    auto external = [&](ull time, Side side, ll price, const std::string& description) {
        snapshot(time, side == Side::Buy ? "external_buy" : "external_sell", description,
                 simulator.submitOrder(2, {ids.next(), side, price, 1}));
    };
    refresh(0, 10000, "Reference 10000: refresh quotes");
    if (name == "inventory-limit") {
        for (ull unit = 1; unit <= 5; ++unit) {
            external(unit * 1000, Side::Sell, 9900, "External trader sells one at limit 9900");
            refresh(unit * 1000, 10000, unit == 5
                    ? "Inventory limit reached: refresh permits ask, suppresses bid"
                    : "Explicit refresh after fill at unchanged reference");
        }
    } else {
        external(1000, Side::Sell, 9900, "External trader sells one at limit 9900");
        external(2000, Side::Buy, 10100, "External trader buys one at limit 10100");
        if (name == "inventory-risk") {
            refresh(3000, 10000, "Reference unchanged: refresh quotes");
            external(4000, Side::Sell, 9900, "External trader sells one at limit 9900");
            refresh(5000, 9500, "Reference drops to 9500: refresh quotes");
        }
    }
    return result;
}

// JSON escaping for arbitrary text; no external serialization dependency.
inline std::string jsonString(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(c) << std::dec;
            } else out << c;
        }
    }
    out << '"';
    return out.str();
}

inline void writeQuoteJson(std::ostream& out, const std::optional<Order>& quote) {
    if (!quote) { out << "null"; return; }
    out << "{\"order_id\":\"" << quote->id << "\",\"price_paise\":\"" << quote->price
        << "\",\"remaining_quantity\":\"" << quote->remaining_qty << "\"}";
}

inline void writeScenarioJson(std::ostream& out, const ScenarioResult& scenario) {
    out << "{\n  \"schema_version\":1,\n  \"scenario\":" << jsonString(scenario.name)
        << ",\n  \"description\":" << jsonString(scenario.description)
        << ",\n  \"units\":{\"money\":\"paise\",\"quantity\":\"units\",\"time\":\"microseconds\"},\n"
        << "  \"assumptions\":[\"Synthetic externally supplied reference price\","
        << "\"No fees, latency, margin, or funding costs\",\"Negative cash and inventory permitted\","
        << "\"No funds reserved for resting orders\",\"No post-only rule or self-trade prevention\","
        << "\"Explicit refresh only; cancel-and-replace loses FIFO priority\","
        << "\"Maker account exclusively strategy-controlled\",\"Snapshots describe state after each event\"],\n"
        << "  \"strategy\":{\"trader_id\":\"1\",\"half_spread_paise\":\"100\","
        << "\"inventory_limit\":\"5\",\"order_quantity\":\"1\"},\n"
        << "  \"initial_accounts\":[{\"trader_id\":\"1\",\"cash_paise\":\"100000\",\"inventory\":\"0\"},"
        << "{\"trader_id\":\"2\",\"cash_paise\":\"100000\",\"inventory\":\"10\"}],\n"
        << "  \"initial_portfolio_value_paise\":\"100000\",\n  \"events\":[\n";
    bool first = true;
    for (const auto& event : scenario.events) {
        if (!first) out << ",\n";
        first = false;
        out << "    {\"event_index\":\"" << event.index << "\",\"timestamp_us\":\"" << event.timestamp_us
            << "\",\"type\":" << jsonString(event.type) << ",\"description\":" << jsonString(event.description)
            << ",\"reference_price_paise\":\"" << event.reference_price << "\",\"maker_bid\":";
        writeQuoteJson(out, event.maker_bid);
        out << ",\"maker_ask\":";
        writeQuoteJson(out, event.maker_ask);
        out << ",\"cash_paise\":\"" << event.maker_account.cash_paise
            << "\",\"inventory\":\"" << event.maker_account.inventory
            << "\",\"portfolio_value_paise\":\"" << event.valuation.portfolio_value_paise
            << "\",\"total_pnl_paise\":\"" << event.valuation.total_pnl_paise << "\",\"trades\":[";
        bool first_trade = true;
        for (const auto& trade : event.trades) {
            if (!first_trade) out << ',';
            first_trade = false;
            out << "{\"buy_id\":\"" << trade.buy_id << "\",\"sell_id\":\"" << trade.sell_id
                << "\",\"price_paise\":\"" << trade.price << "\",\"quantity\":\"" << trade.quantity << "\"}";
        }
        out << "]}";
    }
    out << "\n  ]\n}\n";
}
