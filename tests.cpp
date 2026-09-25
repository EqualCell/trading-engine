#include "order_book.h"

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireTrades(const std::vector<Trade>& actual,
                   const std::vector<Trade>& expected) {
    require(actual.size() == expected.size(), "unexpected trade count");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(actual[i].buy_id == expected[i].buy_id &&
                actual[i].sell_id == expected[i].sell_id &&
                actual[i].price == expected[i].price &&
                actual[i].quantity == expected[i].quantity,
                "trade mismatch at index " + std::to_string(i));
    }
}

void requireInvalid(OrderBook& book, Order order) {
    bool rejected = false;
    try {
        book.addOrder(order);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "expected invalid_argument");
}

std::string bookText(const OrderBook& book) {
    std::ostringstream output;
    auto* original = std::cout.rdbuf(output.rdbuf());
    try {
        book.printBook();
    } catch (...) {
        std::cout.rdbuf(original);
        throw;
    }
    std::cout.rdbuf(original);
    return output.str();
}

void nonCrossingOrders() {
    OrderBook book;
    requireTrades(book.addOrder({1, Side::Buy, 100, 3}), {});
    requireTrades(book.addOrder({2, Side::Sell, 101, 4}), {});
    require(book.bestBid() == 100 && book.bestAsk() == 101 && book.spread() == 1,
            "non-crossing orders should rest on both sides");
    const auto printed = bookText(book);
    require(printed.find("101: [id=2, qty=4]") != std::string::npos &&
            printed.find("100: [id=1, qty=3]") != std::string::npos,
            "BOOK should display resting orders");
}

void buyBestPrice() {
    OrderBook book;
    book.addOrder({1, Side::Sell, 105, 1});
    book.addOrder({2, Side::Sell, 101, 1});
    requireTrades(book.addOrder({3, Side::Buy, 105, 1}), {{3, 2, 101, 1}});
    require(book.bestAsk() == 105, "higher ask should remain");
}

void sellBestPrice() {
    OrderBook book;
    book.addOrder({1, Side::Buy, 100, 1});
    book.addOrder({2, Side::Buy, 104, 1});
    requireTrades(book.addOrder({3, Side::Sell, 100, 1}), {{2, 3, 104, 1}});
    require(book.bestBid() == 100, "lower bid should remain");
}

void fifoAtSamePrice() {
    OrderBook book;
    book.addOrder({1, Side::Sell, 100, 1});
    book.addOrder({2, Side::Sell, 100, 1});
    requireTrades(book.addOrder({3, Side::Buy, 100, 2}),
                  {{3, 1, 100, 1}, {3, 2, 100, 1}});
}

void partialFillRetainsFifo() {
    OrderBook book;
    book.addOrder({1, Side::Sell, 100, 5});
    book.addOrder({2, Side::Sell, 100, 2});
    requireTrades(book.addOrder({3, Side::Buy, 100, 2}), {{3, 1, 100, 2}});
    require(bookText(book).find("100: [id=1, qty=3] [id=2, qty=2]") != std::string::npos,
            "partially filled order should keep its place and remaining quantity");
    requireTrades(book.addOrder({4, Side::Buy, 100, 4}),
                  {{4, 1, 100, 3}, {4, 2, 100, 1}});
    require(book.cancelOrder(2), "partially filled second order should remain indexed");
}

void multipleRestingMatches() {
    OrderBook book;
    book.addOrder({1, Side::Buy, 100, 2});
    book.addOrder({2, Side::Buy, 100, 3});
    book.addOrder({3, Side::Buy, 100, 4});
    requireTrades(book.addOrder({4, Side::Sell, 100, 7}),
                  {{1, 4, 100, 2}, {2, 4, 100, 3}, {3, 4, 100, 2}});
    require(bookText(book).find("100: [id=3, qty=2]") != std::string::npos,
            "last resting order should retain unfilled quantity");
}

void matchesAcrossPrices() {
    OrderBook book;
    book.addOrder({1, Side::Sell, 103, 2});
    book.addOrder({2, Side::Sell, 101, 1});
    book.addOrder({3, Side::Sell, 102, 2});
    requireTrades(book.addOrder({4, Side::Buy, 103, 4}),
                  {{4, 2, 101, 1}, {4, 3, 102, 2}, {4, 1, 103, 1}});
    require(book.bestAsk() == 103 && book.cancelOrder(1),
            "remaining order at final level should be active");
}

void restingPriceSetsTradePrice() {
    OrderBook buyBook;
    buyBook.addOrder({1, Side::Sell, 95, 1});
    requireTrades(buyBook.addOrder({2, Side::Buy, 110, 1}), {{2, 1, 95, 1}});

    OrderBook sellBook;
    sellBook.addOrder({1, Side::Buy, 110, 1});
    requireTrades(sellBook.addOrder({2, Side::Sell, 95, 1}), {{1, 2, 110, 1}});
}

void cancelMiddleOrder() {
    OrderBook book;
    book.addOrder({1, Side::Sell, 100, 1});
    book.addOrder({2, Side::Sell, 100, 1});
    book.addOrder({3, Side::Sell, 100, 1});
    require(book.cancelOrder(2), "middle order should cancel");
    require(bookText(book).find("100: [id=1, qty=1] [id=3, qty=1]") != std::string::npos,
            "cancellation should remove only the middle order");
    requireTrades(book.addOrder({4, Side::Buy, 100, 2}),
                  {{4, 1, 100, 1}, {4, 3, 100, 1}});
}

void cancelFinalAtLevel() {
    OrderBook book;
    book.addOrder({1, Side::Buy, 100, 1});
    book.addOrder({2, Side::Buy, 99, 1});
    book.addOrder({3, Side::Sell, 105, 1});
    require(book.cancelOrder(1), "last order at best bid level should cancel");
    require(book.bestBid() == 99 && book.spread() == 6,
            "empty best bid level should be removed");
    require(book.cancelOrder(3), "last ask should cancel");
    require(!book.bestAsk() && !book.spread(), "empty ask level should be removed");
}

void cancelUnknownAndFilled() {
    OrderBook book;
    require(!book.cancelOrder(999), "unknown order must not cancel");
    book.addOrder({1, Side::Sell, 100, 1});
    book.addOrder({2, Side::Buy, 100, 1});
    require(!book.cancelOrder(1) && !book.cancelOrder(2),
            "filled orders must not be active");
}

void emptyQuote() {
    OrderBook book;
    require(!book.bestBid() && !book.bestAsk() && !book.spread(),
            "empty book quote should have no prices or spread");
}

void oneSidedQuotes() {
    OrderBook book;
    book.addOrder({1, Side::Buy, 100, 1});
    require(book.bestBid() == 100 && !book.bestAsk() && !book.spread(),
            "bid-only quote is wrong");
    require(book.cancelOrder(1), "bid should cancel");
    book.addOrder({2, Side::Sell, 105, 1});
    require(!book.bestBid() && book.bestAsk() == 105 && !book.spread(),
            "ask-only quote is wrong");
}

void twoSidedQuote() {
    OrderBook book;
    book.addOrder({1, Side::Buy, 95, 1});
    book.addOrder({2, Side::Buy, 99, 1});
    book.addOrder({3, Side::Sell, 105, 1});
    book.addOrder({4, Side::Sell, 107, 1});
    require(book.bestBid() == 99 && book.bestAsk() == 105 && book.spread() == 6,
            "two-sided quote should use best levels");
}

void quoteUpdates() {
    OrderBook book;
    book.addOrder({1, Side::Buy, 100, 2});
    book.addOrder({2, Side::Buy, 99, 1});
    book.addOrder({3, Side::Sell, 105, 1});
    book.addOrder({4, Side::Sell, 106, 1});
    requireTrades(book.addOrder({5, Side::Sell, 100, 2}), {{1, 5, 100, 2}});
    require(book.bestBid() == 99 && book.bestAsk() == 105 && book.spread() == 6,
            "quote should update after fill");
    require(book.cancelOrder(3), "best ask should cancel");
    require(book.bestAsk() == 106 && book.spread() == 7,
            "quote should update after cancellation");
}

void invalidPriceAndQuantity() {
    OrderBook book;
    requireInvalid(book, {1, Side::Buy, 0, 1});
    requireInvalid(book, {2, Side::Sell, -1, 1});
    requireInvalid(book, {3, Side::Buy, 100, 0});
    requireInvalid(book, {4, Side::Sell, 100, -1});
    requireTrades(book.addOrder({1, Side::Buy, 100, 1}), {});
    require(book.bestBid() == 100, "invalid order should not reserve its ID");
}

void invalidSide() {
    OrderBook book;
    requireInvalid(book, {1, static_cast<Side>(42), 100, 1});
    requireTrades(book.addOrder({1, Side::Buy, 100, 1}), {});
    require(book.bestBid() == 100, "invalid side should not reserve its ID");
}

void duplicateIdRejection() {
    OrderBook book;
    book.addOrder({1, Side::Buy, 90, 1});
    requireInvalid(book, {1, Side::Sell, 110, 1});
    require(book.cancelOrder(1), "original order should remain active");
    requireInvalid(book, {1, Side::Buy, 90, 1});
    book.addOrder({2, Side::Sell, 100, 1});
    book.addOrder({3, Side::Buy, 100, 1});
    requireInvalid(book, {2, Side::Sell, 100, 1});
    requireInvalid(book, {3, Side::Buy, 100, 1});
    require(!book.bestBid() && !book.bestAsk(),
            "rejected duplicates should not alter the book");
}

int main() {
    using Test = std::pair<const char*, std::function<void()>>;
    const std::vector<Test> tests = {
        {"non-crossing orders", nonCrossingOrders},
        {"best-price priority for incoming buy", buyBestPrice},
        {"best-price priority for incoming sell", sellBestPrice},
        {"FIFO at the same price", fifoAtSamePrice},
        {"partial fills retain FIFO", partialFillRetainsFifo},
        {"multiple resting matches", multipleRestingMatches},
        {"matches across price levels", matchesAcrossPrices},
        {"resting order sets trade price", restingPriceSetsTradePrice},
        {"cancel middle order", cancelMiddleOrder},
        {"cancel final order at level", cancelFinalAtLevel},
        {"cancel unknown and filled orders", cancelUnknownAndFilled},
        {"empty quote", emptyQuote},
        {"one-sided quotes", oneSidedQuotes},
        {"two-sided quote", twoSidedQuote},
        {"quote updates after fills and cancellations", quoteUpdates},
        {"invalid price and quantity", invalidPriceAndQuantity},
        {"invalid side", invalidSide},
        {"duplicate IDs after active, cancelled, and filled orders", duplicateIdRejection},
    };

    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "FAIL: " << name << ": unexpected exception\n";
        }
    }
    return failures == 0 ? 0 : 1;
}
