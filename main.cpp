#include "order_book.h"

#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

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
