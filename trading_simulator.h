#pragma once

#include "order_book.h"

struct Account {
    ll cash_paise;
    ll inventory;
};

class TradingSimulator {
public:
    // Numerical safety bounds, not buying-power or financial risk controls.
    static constexpr ll MAX_INITIAL_CASH_PAISE = 1'000'000'000'000'000'000LL;
    static constexpr ll MAX_INITIAL_INVENTORY = 1'000'000'000LL;
    static constexpr ll MAX_ORDER_PRICE_PAISE = 1'000'000'000LL;
    static constexpr ll MAX_CUMULATIVE_QUANTITY = 1'000'000'000LL;

    void createAccount(ull trader_id, ll initial_cash_paise,
                       ll initial_inventory = 0) {
        if (trader_id == 0) {
            throw std::invalid_argument("Trader ID must be positive");
        }
        if (initial_cash_paise < -MAX_INITIAL_CASH_PAISE ||
            initial_cash_paise > MAX_INITIAL_CASH_PAISE ||
            initial_inventory < -MAX_INITIAL_INVENTORY ||
            initial_inventory > MAX_INITIAL_INVENTORY) {
            throw std::invalid_argument("Initial account exceeds simulator limits");
        }
        if (!accounts.emplace(trader_id,
                              Account{initial_cash_paise, initial_inventory}).second) {
            throw std::invalid_argument("Trader ID already used");
        }
    }

    const Account& getAccount(ull trader_id) const {
        auto found = accounts.find(trader_id);
        if (found == accounts.end()) {
            throw std::invalid_argument("Unknown trader");
        }
        return found->second;
    }

    std::vector<Trade> submitOrder(ull trader_id, Order incoming) {
        getAccount(trader_id);
        if (incoming.id == 0) {
            throw std::invalid_argument("Order ID must be positive");
        }
        // Subtraction avoids overflowing when checking even an LLONG_MAX quantity.
        // The engine still validates positive price/quantity and the side.
        if (incoming.price > MAX_ORDER_PRICE_PAISE ||
            incoming.remaining_qty > MAX_CUMULATIVE_QUANTITY - accepted_quantity) {
            throw std::invalid_argument("Order exceeds simulator limits");
        }
        const auto ownership = owners.emplace(incoming.id, trader_id);
        if (!ownership.second) {
            throw std::invalid_argument("Order ID already used");
        }

        std::vector<Trade> trades;
        try {
            trades = book.addOrder(incoming);
        } catch (const std::invalid_argument&) {
            // These engine validation errors precede matching. Never erase an
            // existing owner, and never assume other exceptions are transactional.
            owners.erase(ownership.first);
            throw;
        }
        // OrderBook has no strong exception guarantee for allocation failures:
        // it may have consumed an ID or executed some fills before throwing.
        // Such a failure is unrecoverable here; callers must discard the simulator.
        accepted_quantity += incoming.remaining_qty;
        for (const Trade& trade : trades) {
            const ull buyer_id = owners.at(trade.buy_id);
            const ull seller_id = owners.at(trade.sell_id);
            // Self-trade prevention is not implemented. Preserve the trade,
            // but its net effect on the single account is zero.
            if (buyer_id == seller_id) {
                continue;
            }
            Account& buyer = accounts.at(buyer_id);
            Account& seller = accounts.at(seller_id);
            const ll notional = trade.price * trade.quantity;
            buyer.cash_paise -= notional;
            buyer.inventory += trade.quantity;
            seller.cash_paise += notional;
            seller.inventory -= trade.quantity;
        }
        return trades;
    }

    bool cancelOrder(ull trader_id, ull order_id) {
        getAccount(trader_id);
        auto found = owners.find(order_id);
        if (found == owners.end() || found->second != trader_id) {
            return false;
        }
        // Historical owners and the submitted quantity budget survive cancellation.
        return book.cancelOrder(order_id);
    }

    const OrderBook& orderBook() const { return book; }

private:
    OrderBook book;
    std::unordered_map<ull, Account> accounts;
    std::unordered_map<ull, ull> owners;
    ll accepted_quantity = 0;
};
