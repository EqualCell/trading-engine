#pragma once

#include "trading_simulator.h"

#include <limits>
#include <optional>
#include <vector>

// All participants in a simulator must share this source and avoid manual IDs.
class OrderIdSource {
public:
    explicit OrderIdSource(ull first = 1) : next_id(first) {
        if (first == 0) throw std::invalid_argument("First order ID must be positive");
    }
    OrderIdSource(const OrderIdSource&) = delete;
    OrderIdSource& operator=(const OrderIdSource&) = delete;

    ull next() {
        if (exhausted) throw std::overflow_error("Order IDs exhausted");
        const ull id = next_id;
        if (id == std::numeric_limits<ull>::max()) exhausted = true;
        else ++next_id;
        return id;
    }

private:
    ull next_id;
    bool exhausted = false;
};

// The trader account must be controlled exclusively by this strategy, with no
// other outstanding orders at construction. One-unit quotes can each fill
// independently without crossing the inventory bounds. Refresh loses FIFO
// priority. There is no post-only rule and no automatic refresh after fills.
class MarketMaker {
public:
    MarketMaker(TradingSimulator& simulator, OrderIdSource& ids, ull trader_id,
                ll half_spread_paise, ll inventory_limit)
        : simulator(simulator), ids(ids), trader_id(trader_id),
          half_spread(half_spread_paise), inventory_limit(inventory_limit) {
        if (trader_id == 0 || half_spread <= 0 ||
            half_spread > (TradingSimulator::MAX_ORDER_PRICE_PAISE - 1) / 2 ||
            inventory_limit < 0) {
            throw std::invalid_argument("Invalid market-maker configuration");
        }
        validateInventory();
    }
    MarketMaker(const MarketMaker&) = delete;
    MarketMaker& operator=(const MarketMaker&) = delete;

    std::vector<Trade> refreshQuotes(ll reference_price) {
        // Compare before adding/subtracting, even for extreme signed inputs.
        if (reference_price <= half_spread ||
            reference_price > TradingSimulator::MAX_ORDER_PRICE_PAISE - half_spread) {
            throw std::invalid_argument("Quote prices outside simulator limits");
        }
        validateInventory();
        // Reserve result space before any fills: at most one trade per unit quote.
        std::vector<Trade> trades;
        trades.reserve(2);
        if (buy_id) simulator.cancelOrder(trader_id, *buy_id);
        if (sell_id) simulator.cancelOrder(trader_id, *sell_id);
        buy_id.reset();
        sell_id.reset();

        // Strict comparisons are equivalent to inventory +/- 1 checks, without
        // arithmetic at the signed limits. Buys always precede sells.
        if (simulator.getAccount(trader_id).inventory < inventory_limit) {
            const ull id = ids.next();
            const auto fills = simulator.submitOrder(
                trader_id, {id, Side::Buy, reference_price - half_spread, 1});
            buy_id = id;
            trades.insert(trades.end(), fills.begin(), fills.end());
        }
        // The buy may have executed immediately. Use the updated inventory.
        if (simulator.getAccount(trader_id).inventory > -inventory_limit) {
            const ull id = ids.next();
            const auto fills = simulator.submitOrder(
                trader_id, {id, Side::Sell, reference_price + half_spread, 1});
            sell_id = id;
            trades.insert(trades.end(), fills.begin(), fills.end());
        }
        return trades;
    }

    // Most recently submitted IDs; a present ID may already have filled.
    std::optional<ull> lastBuyId() const { return buy_id; }
    std::optional<ull> lastSellId() const { return sell_id; }

private:
    void validateInventory() const {
        const ll inventory = simulator.getAccount(trader_id).inventory;
        if (inventory < -inventory_limit || inventory > inventory_limit) {
            throw std::invalid_argument("Maker inventory outside configured limit");
        }
    }
    TradingSimulator& simulator;
    OrderIdSource& ids;
    ull trader_id;
    ll half_spread;
    ll inventory_limit;
    std::optional<ull> buy_id;
    std::optional<ull> sell_id;
};

struct MarkedValuation {
    ll portfolio_value_paise;
    ll total_pnl_paise;
};

// Total P&L marked to an externally supplied synthetic reference price.
// This is neither realized P&L nor liquidation value. No fees, latency, margin,
// or funding costs are modeled. Check each operation, not just account bounds.
inline MarkedValuation markedValuation(const Account& account, ll reference_price,
                                      ll initial_portfolio_value) {
    if (reference_price <= 0) throw std::invalid_argument("Reference price must be positive");
    constexpr ll minimum = std::numeric_limits<ll>::min();
    constexpr ll maximum = std::numeric_limits<ll>::max();
    if (account.inventory > maximum / reference_price ||
        account.inventory < minimum / reference_price) {
        throw std::overflow_error("Marked inventory value overflow");
    }
    const ll inventory_value = account.inventory * reference_price;
    if ((inventory_value > 0 && account.cash_paise > maximum - inventory_value) ||
        (inventory_value < 0 && account.cash_paise < minimum - inventory_value)) {
        throw std::overflow_error("Portfolio value overflow");
    }
    const ll value = account.cash_paise + inventory_value;
    if ((initial_portfolio_value > 0 && value < minimum + initial_portfolio_value) ||
        (initial_portfolio_value < 0 && value > maximum + initial_portfolio_value)) {
        throw std::overflow_error("Total P&L overflow");
    }
    return {value, value - initial_portfolio_value};
}
