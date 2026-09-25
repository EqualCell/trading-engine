#pragma once

#include <algorithm>
#include <functional>
#include <iostream>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using ll = long long;
using ull = unsigned long long;

enum class Side {
    Buy,
    Sell
};

struct Order {
    ull id;
    Side side;
    ll price;          // In paise
    ll remaining_qty;
};

struct Trade {
    ull buy_id;
    ull sell_id;
    ll price;
    ll quantity;
};

class OrderBook {
private:
    // Highest buy price first
    std::map<ll, std::list<Order>, std::greater<ll>> bids;

    // Lowest sell price first
    std::map<ll, std::list<Order>> asks;

    std::unordered_set<ull> used_ids;

    struct OrderLocation {
        Side side;
        ll price;
        std::list<Order>::iterator position;
    };

    // Only orders currently resting in the book belong here.
    std::unordered_map<ull, OrderLocation> active_orders;

    void matchBuy(Order& incoming, std::vector<Trade>& trades) {
        while (incoming.remaining_qty > 0 && !asks.empty()) {
            auto it = asks.begin();

            if (it->first > incoming.price) {
                break;
            }

            Order& resting = it->second.front();

            ll qty = std::min(
                incoming.remaining_qty,
                resting.remaining_qty
            );

            trades.push_back({
                incoming.id,
                resting.id,
                resting.price,
                qty
            });

            incoming.remaining_qty -= qty;
            resting.remaining_qty -= qty;

            if (resting.remaining_qty == 0) {
                active_orders.erase(resting.id);
                it->second.pop_front();
            }

            if (it->second.empty()) {
                asks.erase(it);
            }
        }

        if (incoming.remaining_qty > 0) {
            auto& orders = bids[incoming.price];
            orders.push_back(incoming);
            active_orders.emplace(incoming.id, OrderLocation{
                incoming.side, incoming.price, std::prev(orders.end())
            });
        }
    }

    void matchSell(Order& incoming, std::vector<Trade>& trades) {
        while (incoming.remaining_qty > 0 && !bids.empty()) {
            auto it = bids.begin();

            if (it->first < incoming.price) {
                break;
            }

            Order& resting = it->second.front();

            ll qty = std::min(
                incoming.remaining_qty,
                resting.remaining_qty
            );

            trades.push_back({
                resting.id,
                incoming.id,
                resting.price,
                qty
            });

            incoming.remaining_qty -= qty;
            resting.remaining_qty -= qty;

            if (resting.remaining_qty == 0) {
                active_orders.erase(resting.id);
                it->second.pop_front();
            }

            if (it->second.empty()) {
                bids.erase(it);
            }
        }

        if (incoming.remaining_qty > 0) {
            auto& orders = asks[incoming.price];
            orders.push_back(incoming);
            active_orders.emplace(incoming.id, OrderLocation{
                incoming.side, incoming.price, std::prev(orders.end())
            });
        }
    }

public:
    OrderBook() = default;

    // Copying would leave the index pointing into the original book.
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    std::optional<ll> bestBid() const {
        if (bids.empty()) {
            return std::nullopt;
        }
        return bids.begin()->first;
    }

    std::optional<ll> bestAsk() const {
        if (asks.empty()) {
            return std::nullopt;
        }
        return asks.begin()->first;
    }

    std::optional<ll> spread() const {
        const auto bid = bestBid();
        const auto ask = bestAsk();
        if (!bid || !ask) {
            return std::nullopt;
        }
        return *ask - *bid;
    }

    bool cancelOrder(ull id) {
        auto found = active_orders.find(id);
        if (found == active_orders.end()) {
            return false;
        }

        const OrderLocation& location = found->second;
        if (location.side == Side::Buy) {
            auto level = bids.find(location.price);
            level->second.erase(location.position);
            if (level->second.empty()) {
                bids.erase(level);
            }
        } else {
            auto level = asks.find(location.price);
            level->second.erase(location.position);
            if (level->second.empty()) {
                asks.erase(level);
            }
        }

        active_orders.erase(found);
        // Keep used_ids unchanged: cancelled IDs cannot be reused.
        return true;
    }

    std::vector<Trade> addOrder(Order incoming) {
        if (incoming.price <= 0 || incoming.remaining_qty <= 0) {
            throw std::invalid_argument(
                "Price and quantity must be positive"
            );
        }

        if (incoming.side != Side::Buy &&
            incoming.side != Side::Sell) {
            throw std::invalid_argument("Invalid side");
        }

        if (!used_ids.insert(incoming.id).second) {
            throw std::invalid_argument("Order ID already used");
        }

        std::vector<Trade> trades;

        if (incoming.side == Side::Buy) {
            matchBuy(incoming, trades);
        } else {
            matchSell(incoming, trades);
        }

        return trades;
    }

    void printBook() const {
        std::cout << "\nBOOK (prices in paise)\n";

        std::cout << "ASKS: lowest price first\n";
        for (const auto& [price, orders] : asks) {
            std::cout << price << ": ";

            for (const Order& order : orders) {
                std::cout << "[id=" << order.id
                     << ", qty=" << order.remaining_qty << "] ";
            }

            std::cout << '\n';
        }

        std::cout << "BIDS: highest price first\n";
        for (const auto& [price, orders] : bids) {
            std::cout << price << ": ";

            for (const Order& order : orders) {
                std::cout << "[id=" << order.id
                     << ", qty=" << order.remaining_qty << "] ";
            }

            std::cout << '\n';
        }
    }
};

