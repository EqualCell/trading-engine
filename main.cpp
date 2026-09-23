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
                it->second.pop_front();
            }

            if (it->second.empty()) {
                asks.erase(it);
            }
        }

        if (incoming.remaining_qty > 0) {
            bids[incoming.price].push_back(incoming);
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
                it->second.pop_front();
            }

            if (it->second.empty()) {
                bids.erase(it);
            }
        }

        if (incoming.remaining_qty > 0) {
            asks[incoming.price].push_back(incoming);
        }
    }

public:
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
             << "BOOK\n"
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
            else if (command == "BOOK" ||
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