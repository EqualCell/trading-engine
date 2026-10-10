#include "trading_simulator.h"

#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>

static_assert(std::is_same<decltype(std::declval<TradingSimulator&>().orderBook()),
                           const OrderBook&>::value, "book must be read-only");

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void requireInvalid(const std::function<void()>& action) {
    try { action(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("expected invalid_argument");
}

void balance(const TradingSimulator& sim, ull id, ll cash, ll inventory) {
    const auto& account = sim.getAccount(id);
    require(account.cash_paise == cash && account.inventory == inventory,
            "unexpected balance for trader " + std::to_string(id));
}

void trades(const std::vector<Trade>& actual, const std::vector<Trade>& expected) {
    require(actual.size() == expected.size(), "unexpected trade count");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(actual[i].buy_id == expected[i].buy_id &&
                actual[i].sell_id == expected[i].sell_id &&
                actual[i].price == expected[i].price &&
                actual[i].quantity == expected[i].quantity, "unexpected trade");
    }
}

std::string bookText(const TradingSimulator& sim) {
    std::ostringstream output;
    auto* original = std::cout.rdbuf(output.rdbuf());
    try { sim.orderBook().printBook(); }
    catch (...) { std::cout.rdbuf(original); throw; }
    std::cout.rdbuf(original);
    return output.str();
}

void accounts() {
    TradingSimulator sim;
    sim.createAccount(1, 100);
    balance(sim, 1, 100, 0);
    requireInvalid([&] { sim.createAccount(1, 200, 3); });
    balance(sim, 1, 100, 0);
    requireInvalid([&] { sim.createAccount(0, 0); });
    for (ull id : {0ULL, 9ULL}) {
        requireInvalid([&] { sim.getAccount(id); });
        requireInvalid([&] { sim.submitOrder(id, {1, Side::Buy, 100, 1}); });
        requireInvalid([&] { sim.cancelOrder(id, 1); });
    }
    trades(sim.submitOrder(1, {1, Side::Buy, 100, 2}), {});
    balance(sim, 1, 100, 0);
}

void partialBuyAndCancellation() {
    TradingSimulator sim;
    sim.createAccount(1, 1000);
    sim.createAccount(2, 100, 5);
    trades(sim.submitOrder(2, {10, Side::Sell, 95, 5}), {});
    balance(sim, 2, 100, 5);
    trades(sim.submitOrder(1, {11, Side::Buy, 110, 2}), {{11, 10, 95, 2}});
    balance(sim, 1, 810, 2);
    balance(sim, 2, 290, 3);
    require(bookText(sim).find("[id=10, qty=3]") != std::string::npos,
            "resting remainder wrong");
    require(!sim.cancelOrder(1, 10) && !sim.cancelOrder(1, 999) &&
            !sim.cancelOrder(1, 11) && !sim.cancelOrder(1, 0), "invalid cancellation");
    require(sim.cancelOrder(2, 10) && !sim.cancelOrder(2, 10), "cancel remainder");
    balance(sim, 1, 810, 2);
    balance(sim, 2, 290, 3);
    require(!sim.orderBook().bestAsk(), "cancel should remove ask");
    requireInvalid([&] { sim.submitOrder(1, {10, Side::Buy, 100, 1}); });
    requireInvalid([&] { sim.submitOrder(2, {11, Side::Sell, 100, 1}); });
}

void basicFill() {
    TradingSimulator sim;
    sim.createAccount(1, 1000, 1);
    sim.createAccount(2, 50, 4);
    sim.submitOrder(2, {1, Side::Sell, 100, 3});
    trades(sim.submitOrder(1, {2, Side::Buy, 100, 3}), {{2, 1, 100, 3}});
    balance(sim, 1, 700, 4);
    balance(sim, 2, 350, 1);
    require(!sim.cancelOrder(2, 1) && !sim.cancelOrder(1, 2), "filled orders inactive");
}

void multipleLevelsAndConservation() {
    TradingSimulator sim;
    for (ull id : {1ULL, 2ULL, 3ULL}) sim.createAccount(id, 0);
    sim.submitOrder(2, {1, Side::Sell, 100, 2});
    sim.submitOrder(3, {2, Side::Sell, 105, 3});
    trades(sim.submitOrder(1, {3, Side::Buy, 110, 4}),
           {{3, 1, 100, 2}, {3, 2, 105, 2}});
    balance(sim, 1, -410, 4);
    balance(sim, 2, 200, -2);
    balance(sim, 3, 210, -2);
    sim.submitOrder(2, {4, Side::Buy, 90, 2});
    sim.submitOrder(1, {5, Side::Buy, 95, 1});
    trades(sim.submitOrder(3, {6, Side::Sell, 85, 4}),
           {{5, 6, 95, 1}, {4, 6, 90, 2}});
    balance(sim, 1, -505, 5);
    balance(sim, 2, 20, 0);
    balance(sim, 3, 485, -5);
    require(bookText(sim).find("[id=6, qty=1]") != std::string::npos,
            "incoming sell remainder wrong");
    ll cash = 0, inventory = 0;
    for (ull id : {1ULL, 2ULL, 3ULL}) {
        cash += sim.getAccount(id).cash_paise;
        inventory += sim.getAccount(id).inventory;
    }
    require(cash == 0 && inventory == 0, "cash/inventory conservation");
}

void rejectionAndOwnership() {
    TradingSimulator sim;
    sim.createAccount(1, 1000, 10);
    sim.createAccount(2, 2000, 20);
    sim.submitOrder(1, {1, Side::Sell, 100, 5});
    const auto before = bookText(sim);
    const std::vector<Order> invalid = {
        {0, Side::Buy, 100, 1}, {2, Side::Buy, 0, 1},
        {2, Side::Buy, -1, 1}, {2, Side::Buy, 100, 0},
        {2, Side::Buy, 100, -1}, {2, static_cast<Side>(42), 100, 1},
        {1, Side::Buy, 100, 1}, {2, Side::Buy, 1'000'000'001, 1},
        {2, Side::Buy, 100, std::numeric_limits<ll>::max()},
        {2, Side::Buy, std::numeric_limits<ll>::max(), 1},
        {2, Side::Buy, std::numeric_limits<ll>::min(), 1},
        {2, Side::Buy, 100, std::numeric_limits<ll>::min()}
    };
    for (const auto& order : invalid) {
        requireInvalid([&] { sim.submitOrder(2, order); });
        require(bookText(sim) == before, "rejection changed book");
        balance(sim, 1, 1000, 10);
        balance(sim, 2, 2000, 20);
    }
    require(!sim.cancelOrder(2, 1), "duplicate overwrote ownership");
    trades(sim.submitOrder(2, {2, Side::Buy, 100, 5}), {{2, 1, 100, 5}});
    balance(sim, 1, 1500, 5);
    balance(sim, 2, 1500, 25);
    requireInvalid([&] { sim.submitOrder(2, {1, Side::Buy, 100, 1}); });
    requireInvalid([&] { sim.submitOrder(1, {2, Side::Sell, 100, 1}); });
}

void selfTrades() {
    TradingSimulator sim;
    sim.createAccount(1, -100, -2);
    sim.submitOrder(1, {1, Side::Sell, 100, 3});
    trades(sim.submitOrder(1, {2, Side::Buy, 110, 2}), {{2, 1, 100, 2}});
    balance(sim, 1, -100, -2);
    sim.submitOrder(1, {3, Side::Buy, 90, 2});
    trades(sim.submitOrder(1, {4, Side::Sell, 80, 2}), {{3, 4, 90, 2}});
    balance(sim, 1, -100, -2);
    require(sim.cancelOrder(1, 1), "self trade remainder active");
}

void numericBoundaries() {
    TradingSimulator sim;
    constexpr ll cash = TradingSimulator::MAX_INITIAL_CASH_PAISE;
    constexpr ll qty = TradingSimulator::MAX_INITIAL_INVENTORY;
    constexpr ll price = TradingSimulator::MAX_ORDER_PRICE_PAISE;
    sim.createAccount(1, -cash, qty);
    sim.createAccount(2, cash, -qty);
    for (ll value : {-cash - 1, cash + 1, std::numeric_limits<ll>::min(),
                     std::numeric_limits<ll>::max()}) {
        requireInvalid([&] { sim.createAccount(3, value); });
    }
    for (ll value : {-qty - 1, qty + 1, std::numeric_limits<ll>::min(),
                     std::numeric_limits<ll>::max()}) {
        requireInvalid([&] { sim.createAccount(3, 0, value); });
    }
    sim.createAccount(3, 0);
    sim.submitOrder(2, {1, Side::Sell, price, qty / 2});
    const auto before = bookText(sim);
    requireInvalid([&] { sim.submitOrder(1, {2, Side::Buy, price + 1, qty / 2}); });
    requireInvalid([&] { sim.submitOrder(1, {2, Side::Buy, price, qty / 2 + 1}); });
    require(bookText(sim) == before, "boundary rejection mutated book");
    trades(sim.submitOrder(1, {2, Side::Buy, price, qty / 2}),
           {{2, 1, price, qty / 2}});
    balance(sim, 1, -cash - cash / 2, qty + qty / 2);
    balance(sim, 2, cash + cash / 2, -qty - qty / 2);
    requireInvalid([&] { sim.submitOrder(3, {3, Side::Buy, 1, 1}); });
    require(!sim.orderBook().bestBid() && !sim.orderBook().bestAsk(), "budget mutation");
}

void quantityBudget() {
    TradingSimulator sim;
    sim.createAccount(1, 0);
    sim.createAccount(2, 0);
    constexpr ll limit = TradingSimulator::MAX_CUMULATIVE_QUANTITY;
    requireInvalid([&] { sim.submitOrder(1, {1, static_cast<Side>(42), 1, limit}); });
    requireInvalid([&] { sim.submitOrder(1, {1, Side::Buy, 0, limit}); });
    requireInvalid([&] { sim.submitOrder(1, {1, Side::Buy, 1, limit + 1}); });
    sim.submitOrder(1, {1, Side::Buy, 1, limit - 1});
    requireInvalid([&] { sim.submitOrder(2, {1, Side::Sell, 1, 1}); });
    require(sim.cancelOrder(1, 1), "large order cancellation");
    sim.submitOrder(2, {2, Side::Sell, 1, 1});
    const auto before = bookText(sim);
    requireInvalid([&] { sim.submitOrder(1, {3, Side::Buy, 1, 1}); });
    require(bookText(sim) == before, "cancellation restored budget");
    balance(sim, 1, 0, 0);
    balance(sim, 2, 0, 0);

    TradingSimulator exact;
    exact.createAccount(1, 0);
    trades(exact.submitOrder(1, {1, Side::Buy, 1, limit}), {});
    requireInvalid([&] { exact.submitOrder(1, {2, Side::Buy, 1, 1}); });
}

int main() {
    using Test = std::pair<const char*, std::function<void()>>;
    const std::vector<Test> tests = {
        {"accounts and unknown traders", accounts},
        {"basic fill", basicFill},
        {"partial buy and cancellation", partialBuyAndCancellation},
        {"multiple levels, aggressive sells, and conservation", multipleLevelsAndConservation},
        {"rejection and historical ownership", rejectionAndOwnership},
        {"self trades", selfTrades},
        {"numeric boundaries", numericBoundaries},
        {"cumulative quantity budget", quantityBudget}
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
