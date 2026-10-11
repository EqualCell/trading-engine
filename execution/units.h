#pragma once
#include "../order_book.h"
#include <limits>
#include <string>

namespace execution {
constexpr ll MAX_PRICE = 1000000000, MAX_QUANTITY = 1000000000;
inline void require(bool ok, const std::string& message) { if (!ok) throw std::invalid_argument(message); }
inline ll add(ll a, ll b) {
    if ((b > 0 && a > std::numeric_limits<ll>::max() - b) ||
        (b < 0 && a < std::numeric_limits<ll>::min() - b)) throw std::overflow_error("Integer addition overflow");
    return a + b;
}
inline ll product(ll a, ll b) {
    require(a >= 0 && b >= 0, "Expected nonnegative product");
    if (b && a > std::numeric_limits<ll>::max() / b) throw std::overflow_error("Integer product overflow");
    return a * b;
}
inline ll decimalScale(unsigned places) {
    require(places <= 18, "Decimal scale exceeds signed 64-bit support (18 places)");
    ll scale = 1; while (places--) scale = product(scale, 10); return scale;
}
// Nonnegative plain decimal syntax: digits, optionally '.' followed by digits.
// Leading zeroes are accepted. Extra precision is allowed only for trailing zeroes.
inline ll parseDecimal(const std::string& text, unsigned places) {
    decimalScale(places);
    require(!text.empty(), "Missing decimal");
    const auto dot = text.find('.');
    const std::size_t whole = dot == std::string::npos ? text.size() : dot;
    const std::size_t fraction = dot == std::string::npos ? 0 : text.size() - dot - 1;
    require(whole > 0 && (dot == std::string::npos || fraction > 0), "Malformed plain decimal");
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == dot) continue;
        require(text[i] >= '0' && text[i] <= '9', "Invalid nonnegative plain decimal");
    }
    for (std::size_t i = places; i < fraction; ++i) require(text[whole + 1 + i] == '0', "Nonzero decimal beyond supported precision");
    ll value = 0;
    for (std::size_t i = 0; i < whole; ++i) value = add(product(value, 10), text[i] - '0');
    for (unsigned i = 0; i < places; ++i) value = add(product(value, 10), i < fraction ? text[whole + 1 + i] - '0' : 0);
    return value;
}
inline std::string formatDecimal(ll atoms, unsigned places) {
    require(atoms >= 0, "Cannot format negative execution amount");
    const ll scale = decimalScale(places);
    std::string out = std::to_string(atoms / scale);
    if (places) {
        std::string fraction = std::to_string(atoms % scale);
        fraction.insert(0, places - fraction.size(), '0');
        while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
        if (!fraction.empty()) out += '.' + fraction;
    }
    return out;
}
struct Instrument {
    std::string symbol = "WHOLE", base = "UNIT", quote = "INR";
    unsigned price_decimals = 2, quantity_decimals = 0;
    ll price_tick_atoms = 1, quantity_step_atoms = 1;
    // Adapter safety limits; independent from representation precision.
    ll max_price_atoms = MAX_PRICE, max_quantity_atoms = MAX_QUANTITY;
    unsigned cashDecimals() const { validate(); return price_decimals + quantity_decimals; }
    void validate() const {
        for (const auto& label : {symbol, base, quote}) {
            require(!label.empty() && label.size() <= 64, "Invalid instrument label");
            for (char c : label) require((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_', "Invalid instrument label character");
        }
        require(price_decimals <= 18 && quantity_decimals <= 18 && price_decimals + quantity_decimals <= 18, "Unsupported instrument scales: cash places must fit 0..18");
        require(max_price_atoms > 0 && max_quantity_atoms > 0 && price_tick_atoms > 0 && quantity_step_atoms > 0 && price_tick_atoms <= max_price_atoms && quantity_step_atoms <= max_quantity_atoms, "Invalid instrument increments/bounds");
    }
    ll maximumPrice() const { validate(); return max_price_atoms - max_price_atoms % price_tick_atoms; }
    void validatePrice(ll price) const { validate(); require(price > 0 && price <= max_price_atoms && price % price_tick_atoms == 0, "Price outside bounds or off tick"); }
    void validateQuantity(ll quantity) const { validate(); require(quantity > 0 && quantity <= max_quantity_atoms && quantity % quantity_step_atoms == 0, "Quantity outside bounds or off step"); }
    void validateOrder(ll price, ll quantity) const { validatePrice(price); validateQuantity(quantity); product(price, quantity); }
    ll price(const std::string& text) const { validate(); return parseDecimal(text, price_decimals); }
    ll quantity(const std::string& text) const { validate(); return parseDecimal(text, quantity_decimals); }
    ll cash(const std::string& text) const { return parseDecimal(text, cashDecimals()); }
    std::string priceText(ll value) const { validate(); return formatDecimal(value, price_decimals); }
    std::string quantityText(ll value) const { validate(); return formatDecimal(value, quantity_decimals); }
    std::string cashText(ll value) const { return formatDecimal(value, cashDecimals()); }
    bool legacyUnits() const { return symbol == "WHOLE" && base == "UNIT" && quote == "INR" && price_decimals == 2 && quantity_decimals == 0; }
};
inline Instrument illustrativeBTC() { return {"BTCUSDT-DEMO", "BTC", "USDT", 2, 8, 1, 1000}; }
}
