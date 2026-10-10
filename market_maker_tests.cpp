#include "market_maker_scenarios.h"

#include <functional>
#include <iostream>
#include <sstream>
#include <string>

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Exception = std::invalid_argument>
void requireThrows(const std::function<void()>& action) {
    try { action(); }
    catch (const Exception&) { return; }
    throw std::runtime_error("expected exception");
}

void requireTrades(const std::vector<Trade>& actual, const std::vector<Trade>& expected) {
    require(actual.size() == expected.size(), "unexpected trade count");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(actual[i].buy_id == expected[i].buy_id &&
                actual[i].sell_id == expected[i].sell_id &&
                actual[i].price == expected[i].price &&
                actual[i].quantity == expected[i].quantity, "trade mismatch");
    }
}

void balance(const TradingSimulator& sim, ll cash, ll inventory) {
    const auto& account = sim.getAccount(1);
    require(account.cash_paise == cash && account.inventory == inventory,
            "maker balance mismatch");
}

std::string bookText(const TradingSimulator& sim) {
    std::ostringstream output;
    auto* original = std::cout.rdbuf(output.rdbuf());
    try { sim.orderBook().printBook(); }
    catch (...) { std::cout.rdbuf(original); throw; }
    std::cout.rdbuf(original);
    return output.str();
}

void scenario() {
    TradingSimulator sim;
    sim.createAccount(1, 100000);
    sim.createAccount(2, 100000, 10);
    OrderIdSource ids;
    MarketMaker maker(sim, ids, 1, 100, 5);
    requireTrades(maker.refreshQuotes(10000), {});
    require(sim.orderBook().bestBid() == 9900 && sim.orderBook().bestAsk() == 10100,
            "initial quotes");
    require(maker.lastBuyId() == 1 && maker.lastSellId() == 2, "buy-before-sell IDs");
    requireTrades(sim.submitOrder(2, {ids.next(), Side::Sell, 9900, 1}), {{1, 3, 9900, 1}});
    balance(sim, 90100, 1);
    require(!sim.orderBook().bestBid() && sim.orderBook().bestAsk() == 10100,
            "must not replenish on fill");
    requireTrades(sim.submitOrder(2, {ids.next(), Side::Buy, 10100, 1}), {{4, 2, 10100, 1}});
    balance(sim, 100200, 0);
    const auto round_trip = markedValuation(sim.getAccount(1), 10000, 100000);
    require(round_trip.portfolio_value_paise == 100200 && round_trip.total_pnl_paise == 200,
            "round-trip mark");
    requireTrades(maker.refreshQuotes(10000), {}); // Both old quotes filled.
    require(maker.lastBuyId() == 5 && maker.lastSellId() == 6, "shared monotonic IDs");
    requireTrades(sim.submitOrder(2, {ids.next(), Side::Sell, 9900, 1}), {{5, 7, 9900, 1}});
    balance(sim, 90300, 1);
    requireTrades(maker.refreshQuotes(9500), {}); // One old quote filled.
    require(maker.lastBuyId() == 8 && maker.lastSellId() == 9, "refresh IDs");
    require(!sim.cancelOrder(1, 6), "old sell quote not removed");
    require(sim.orderBook().bestBid() == 9400 && sim.orderBook().bestAsk() == 9600,
            "new quotes after drop");
    const auto final = markedValuation(sim.getAccount(1), 9500, 100000);
    require(final.portfolio_value_paise == 99800 && final.total_pnl_paise == -200,
            "inventory risk mark");
}

void replacementAndFifo() {
    TradingSimulator sim;
    sim.createAccount(1, 0);
    sim.createAccount(2, 0);
    OrderIdSource ids;
    MarketMaker maker(sim, ids, 1, 10, 2);
    maker.refreshQuotes(100);
    const ull old_buy = *maker.lastBuyId(), old_sell = *maker.lastSellId();
    const ull external = ids.next();
    sim.submitOrder(2, {external, Side::Buy, 90, 1});
    maker.refreshQuotes(100);
    require(!sim.cancelOrder(1, old_buy) && !sim.cancelOrder(1, old_sell), "old quotes active");
    const ull incoming = ids.next();
    requireTrades(sim.submitOrder(2, {incoming, Side::Sell, 90, 1}),
                  {{external, incoming, 90, 1}});
    balance(sim, 0, 0); // External order gained priority over replacement maker buy.
    require(sim.orderBook().bestBid() == 90 && sim.orderBook().bestAsk() == 110,
            "replacement quotes missing");
    const auto text = bookText(sim);
    require(text.find("[id=" + std::to_string(*maker.lastBuyId()) + ", qty=1]") != std::string::npos,
            "replacement buy not one unit");
}

void independentInventoryLimits() {
    // At each bound only the inventory-reducing side may rest. Exercise each
    // side independently from the interior, including refresh after a fill.
    for (ll initial : {-2LL, 2LL}) {
        TradingSimulator sim;
        sim.createAccount(1, 0, initial);
        sim.createAccount(2, 0);
        OrderIdSource ids;
        MarketMaker maker(sim, ids, 1, 10, 2);
        maker.refreshQuotes(100);
        require((initial == 2 ? !maker.lastBuyId() : !maker.lastSellId()), "bound suppression");
        require((initial == 2 ? !sim.orderBook().bestBid() : !sim.orderBook().bestAsk()),
                "forbidden side resting");
    }
    for (Side external_side : {Side::Buy, Side::Sell}) {
        TradingSimulator sim;
        sim.createAccount(1, 0);
        sim.createAccount(2, 0);
        OrderIdSource ids;
        MarketMaker maker(sim, ids, 1, 10, 1);
        maker.refreshQuotes(100);
        const ll price = external_side == Side::Sell ? 90 : 110;
        const auto fills = sim.submitOrder(2, {ids.next(), external_side, price, 1});
        require(fills.size() == 1, "independent fill missing");
        const ll inventory = external_side == Side::Sell ? 1 : -1;
        require(sim.getAccount(1).inventory == inventory, "independent fill crossed bound");
        maker.refreshQuotes(100);
        require(inventory == 1 ? !maker.lastBuyId() : !maker.lastSellId(), "refresh at bound");
    }
    TradingSimulator flat;
    flat.createAccount(1, 0);
    OrderIdSource ids;
    MarketMaker zero(flat, ids, 1, 10, 0);
    requireTrades(zero.refreshQuotes(100), {});
    require(!zero.lastBuyId() && !zero.lastSellId(), "zero limit should quote neither side");
}

void immediateExecutions() {
    TradingSimulator sim;
    sim.createAccount(1, 1000, -1);
    sim.createAccount(2, 0);
    OrderIdSource ids;
    const ull external_ask = ids.next();
    sim.submitOrder(2, {external_ask, Side::Sell, 80, 1});
    MarketMaker maker(sim, ids, 1, 10, 1);
    requireTrades(maker.refreshQuotes(100), {{2, external_ask, 80, 1}});
    // Buy moved inventory from -limit to zero, enabling the subsequent sell.
    balance(sim, 920, 0);
    require(maker.lastSellId() == 3 && sim.orderBook().bestAsk() == 110,
            "sell decision used stale inventory");
    require(!sim.orderBook().bestBid(), "immediately filled buy still resting");

    TradingSimulator other;
    other.createAccount(1, 1000, 1);
    other.createAccount(2, 0);
    OrderIdSource other_ids;
    const ull external_bid = other_ids.next();
    other.submitOrder(2, {external_bid, Side::Buy, 120, 1});
    MarketMaker seller(other, other_ids, 1, 10, 1);
    requireTrades(seller.refreshQuotes(100), {{external_bid, 2, 120, 1}});
    balance(other, 1120, 0);
    require(!seller.lastBuyId() && seller.lastSellId() == 2, "buy suppressed at initial bound");
    require(!other.orderBook().bestBid() && !other.orderBook().bestAsk(), "immediate sell resting");
}

void invalidInputs() {
    TradingSimulator sim;
    sim.createAccount(1, 0);
    sim.createAccount(2, 0, 3);
    OrderIdSource ids;
    for (ll half : {0LL, -1LL, std::numeric_limits<ll>::max()}) {
        requireThrows([&] { MarketMaker invalid(sim, ids, 1, half, 5); });
    }
    requireThrows([&] { MarketMaker invalid(sim, ids, 0, 10, 5); });
    requireThrows([&] { MarketMaker invalid(sim, ids, 99, 10, 5); });
    requireThrows([&] { MarketMaker invalid(sim, ids, 1, 10, -1); });
    requireThrows([&] { MarketMaker invalid(sim, ids, 1, 10, std::numeric_limits<ll>::min()); });
    requireThrows([&] { MarketMaker invalid(sim, ids, 2, 10, 2); });
    MarketMaker maker(sim, ids, 1, 10, 5);
    maker.refreshQuotes(100);
    const auto before = bookText(sim);
    for (ll reference : {0LL, -1LL, 10LL, 1'000'000'000LL,
                         std::numeric_limits<ll>::min(), std::numeric_limits<ll>::max()}) {
        requireThrows([&] { maker.refreshQuotes(reference); });
        require(bookText(sim) == before, "invalid price cancelled quotes");
        require(maker.lastBuyId() == 1 && maker.lastSellId() == 2, "invalid price changed IDs");
        balance(sim, 0, 0);
    }
    maker.refreshQuotes(11);
    require(sim.orderBook().bestBid() == 1 && sim.orderBook().bestAsk() == 21, "lower boundary");
    maker.refreshQuotes(TradingSimulator::MAX_ORDER_PRICE_PAISE - 10);
    require(sim.orderBook().bestAsk() == TradingSimulator::MAX_ORDER_PRICE_PAISE, "upper boundary");
}

void idExhaustion() {
    requireThrows([&] { OrderIdSource invalid(0); });
    const ull maximum = std::numeric_limits<ull>::max();
    OrderIdSource ids(maximum - 1);
    require(ids.next() == maximum - 1 && ids.next() == maximum, "ID boundary");
    requireThrows<std::overflow_error>([&] { ids.next(); });
    requireThrows<std::overflow_error>([&] { ids.next(); });
}

void checkedValuation() {
    constexpr ll maximum = std::numeric_limits<ll>::max();
    constexpr ll minimum = std::numeric_limits<ll>::min();
    require(markedValuation({100, -2}, 10, 100).total_pnl_paise == -20, "short valuation");
    require(markedValuation({0, minimum}, 1, 0).portfolio_value_paise == minimum, "minimum mark");
    require(markedValuation({maximum, 0}, 1, maximum).total_pnl_paise == 0, "maximum mark");
    requireThrows([&] { markedValuation({0, 0}, 0, 0); });
    requireThrows([&] { markedValuation({0, 0}, -1, 0); });
    requireThrows<std::overflow_error>([&] { markedValuation({0, maximum}, 2, 0); });
    requireThrows<std::overflow_error>([&] { markedValuation({0, minimum}, 2, 0); });
    requireThrows<std::overflow_error>([&] { markedValuation({maximum, 1}, 1, 0); });
    requireThrows<std::overflow_error>([&] { markedValuation({minimum, -1}, 1, 0); });
    requireThrows<std::overflow_error>([&] { markedValuation({minimum, 0}, 1, 1); });
    requireThrows<std::overflow_error>([&] { markedValuation({maximum, 0}, 1, -1); });
}

void exportPrimitivesAndImmediateFills() {
    require(jsonString("\"\\\n\r\t" + std::string(1, '\1')) ==
            "\"\\\"\\\\\\n\\r\\t\\u0001\"", "JSON string escaping");
    TradingSimulator sim;
    sim.createAccount(1, 100000, 0);
    sim.createAccount(2, 100000, 10);
    OrderIdSource ids;
    sim.submitOrder(2, {ids.next(), Side::Sell, 9800, 1});
    MarketMaker maker(sim, ids, 1, 100, 5);
    const auto fills = maker.refreshQuotes(10000);
    requireTrades(fills, {{2, 1, 9800, 1}});
    require(!sim.orderBook().activeOrder(*maker.lastBuyId()), "immediate buy exported as active");
    const auto ask = sim.orderBook().activeOrder(*maker.lastSellId());
    require(ask && ask->remaining_qty == 1 && ask->price == 10100, "active maker ask missing");
    std::ostringstream quote;
    writeQuoteJson(quote, sim.orderBook().activeOrder(*maker.lastBuyId()));
    require(quote.str() == "null", "filled quote JSON");
    ScenarioResult result;
    result.name = "immediate-fill";
    result.description = "Immediate execution during strategy refresh";
    const Account account = sim.getAccount(1);
    result.events.push_back({0, 0, "quote_refresh", result.description, 10000,
                             std::nullopt, ask, account,
                             markedValuation(account, 10000, 100000), fills});
    std::ostringstream output;
    writeScenarioJson(output, result);
    require(output.str().find("\"buy_id\":\"2\",\"sell_id\":\"1\",\"price_paise\":\"9800\",\"quantity\":\"1\"") != std::string::npos,
            "immediate execution lost in JSON serialization");
}

int main() {
    using Test = std::pair<const char*, std::function<void()>>;
    const std::vector<Test> tests = {
        {"deterministic spread and inventory-risk scenario", scenario},
        {"refresh removes quotes and loses FIFO priority", replacementAndFifo},
        {"independent inventory limits", independentInventoryLimits},
        {"immediate executions and updated inventory", immediateExecutions},
        {"invalid configuration and quote prices", invalidInputs},
        {"ID exhaustion without wrapping", idExhaustion},
        {"checked marked valuation", checkedValuation},
        {"JSON escaping, active quotes, and immediate executions", exportPrimitivesAndImmediateFills}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
        catch (...) {
            ++failures;
            std::cerr << "FAIL: " << name << ": unexpected exception\n";
        }
    }
    return failures == 0 ? 0 : 1;
}
