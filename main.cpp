#include <bits/stdc++.h>
using namespace std;

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
    map<ll, list<Order>, greater<ll>> bids;

    // Lowest sell price first
    map<ll, list<Order>> asks;

    unordered_set<ull> used_ids;

    struct OrderLocation {
        Side side;
        ll price;
        list<Order>::iterator position;
    };

    // Only orders currently resting in the book belong here.
    unordered_map<ull, OrderLocation> active_orders;

    void matchBuy(Order& incoming, vector<Trade>& trades) {
        while (incoming.remaining_qty > 0 && !asks.empty()) {
            auto it = asks.begin();

            if (it->first > incoming.price) {
                break;
            }

            Order& resting = it->second.front();

            ll qty = min(
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
                incoming.side, incoming.price, prev(orders.end())
            });
        }
    }

    void matchSell(Order& incoming, vector<Trade>& trades) {
        while (incoming.remaining_qty > 0 && !bids.empty()) {
            auto it = bids.begin();

            if (it->first < incoming.price) {
                break;
            }

            Order& resting = it->second.front();

            ll qty = min(
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
                incoming.side, incoming.price, prev(orders.end())
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

    optional<ll> bestBid() const {
        if (bids.empty()) {
            return nullopt;
        }
        return bids.begin()->first;
    }

    optional<ll> bestAsk() const {
        if (asks.empty()) {
            return nullopt;
        }
        return asks.begin()->first;
    }

    optional<ll> spread() const {
        const auto bid = bestBid();
        const auto ask = bestAsk();
        if (!bid || !ask) {
            return nullopt;
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

    vector<Trade> addOrder(Order incoming) {
        if (incoming.price <= 0 || incoming.remaining_qty <= 0) {
            throw invalid_argument(
                "Price and quantity must be positive"
            );
        }

        if (incoming.side != Side::Buy &&
            incoming.side != Side::Sell) {
            throw invalid_argument("Invalid side");
        }

        if (!used_ids.insert(incoming.id).second) {
            throw invalid_argument("Order ID already used");
        }

        vector<Trade> trades;

        if (incoming.side == Side::Buy) {
            matchBuy(incoming, trades);
        } else {
            matchSell(incoming, trades);
        }

        return trades;
    }

    void printBook() const {
        cout << "\nBOOK (prices in paise)\n";

        cout << "ASKS: lowest price first\n";
        for (const auto& [price, orders] : asks) {
            cout << price << ": ";

            for (const Order& order : orders) {
                cout << "[id=" << order.id
                     << ", qty=" << order.remaining_qty << "] ";
            }

            cout << '\n';
        }

        cout << "BIDS: highest price first\n";
        for (const auto& [price, orders] : bids) {
            cout << price << ": ";

            for (const Order& order : orders) {
                cout << "[id=" << order.id
                     << ", qty=" << order.remaining_qty << "] ";
            }

            cout << '\n';
        }
    }
};

int main() {
    OrderBook book;
    string line;

    auto printHelp = []() {
        cout << "\nCommands:\n"
             << "BUY id price quantity\n"
             << "SELL id price quantity\n"
             << "CANCEL id\n"
             << "BOOK\n"
             << "QUOTE\n"
             << "HELP\n"
             << "EXIT\n"
             << "Prices are in paise: Rs 100 = 10000\n\n";
    };

    // Accept only positive integers that fit in long long.
    auto parsePositive = [](const string& s) -> ll {
        if (s.empty()) {
            throw invalid_argument("Missing number");
        }

        for (char c : s) {
            if (c < '0' || c > '9') {
                throw invalid_argument("Use positive whole numbers");
            }
        }

        ll value = stoll(s);

        if (value <= 0) {
            throw invalid_argument("Numbers must be positive");
        }

        return value;
    };

    printHelp();

    while (true) {
        cout << "> " << flush;

        if (!getline(cin, line)) {
            break;
        }

        stringstream ss(line);
        string command;
        ss >> command;

        if (command.empty()) {
            continue;
        }

        // Allow lowercase commands too.
        for (char& c : command) {
            c = static_cast<char>(
                toupper(static_cast<unsigned char>(c))
            );
        }

        try {
            if (command == "BUY" || command == "SELL") {
                string idText, priceText, qtyText, extra;

                if (!(ss >> idText >> priceText >> qtyText) ||
                    (ss >> extra)) {
                    cout << "Usage: " << command
                         << " id price quantity\n";
                    continue;
                }

                ull id = static_cast<ull>(parsePositive(idText));
                ll price = parsePositive(priceText);
                ll qty = parsePositive(qtyText);

                Side side = (command == "BUY")
                            ? Side::Buy : Side::Sell;

                vector<Trade> trades = book.addOrder({
                    id, side, price, qty
                });

                ll filled = 0;

                cout << "ACCEPTED order " << id << '\n';

                for (const Trade& t : trades) {
                    cout << "TRADE buy=" << t.buy_id
                         << " sell=" << t.sell_id
                         << " price=" << t.price
                         << " qty=" << t.quantity << '\n';

                    filled += t.quantity;
                }

                cout << "Filled: " << filled
                     << " | Resting: " << qty - filled << '\n';
            }
            else if (command == "CANCEL") {
                string idText, extra;
                if (!(ss >> idText) || (ss >> extra)) {
                    cout << "Usage: CANCEL id\n";
                    continue;
                }

                ull id = static_cast<ull>(parsePositive(idText));
                if (book.cancelOrder(id)) {
                    cout << "CANCELLED order " << id << '\n';
                } else {
                    cout << "REJECTED: no active order with ID " << id << '\n';
                }
            }
            else if (command == "BOOK" ||
                     command == "QUOTE" ||
                     command == "HELP" ||
                     command == "EXIT") {
                string extra;

                if (ss >> extra) {
                    cout << command << " takes no arguments\n";
                    continue;
                }

                if (command == "BOOK") {
                    book.printBook();
                }
                else if (command == "QUOTE") {
                    const auto bid = book.bestBid();
                    const auto ask = book.bestAsk();
                    const auto spread = book.spread();
                    cout << "QUOTE (prices in paise)\n"
                         << "Best bid: " << (bid ? to_string(*bid) : "N/A") << '\n'
                         << "Best ask: " << (ask ? to_string(*ask) : "N/A") << '\n'
                         << "Spread: " << (spread ? to_string(*spread) : "N/A") << '\n';
                }
                else if (command == "HELP") {
                    printHelp();
                }
                else {
                    break;
                }
            }
            else {
                cout << "Unknown command. Type HELP.\n";
            }
        }
        catch (const invalid_argument& e) {
            cout << "REJECTED: " << e.what() << '\n';
        }
        catch (const out_of_range&) {
            cout << "REJECTED: number is too large\n";
        }
    }

    return 0;
}
