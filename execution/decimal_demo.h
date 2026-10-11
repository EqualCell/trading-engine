#pragma once
#include "experiment.h"
namespace execution {
inline std::string fractionalDemo(bool scheduled = false, bool partial = false) {
    const Instrument spec = illustrativeBTC();
    const Task task{Side::Buy, spec.quantity("0.00123000"), 10, 100, spec.price("43010.00")};
    Experiment run(task, {spec.cash("1000"), 0}, 2, 5, {scheduled ? PolicyKind::Scheduled : PolicyKind::Immediate, scheduled ? 5 : 1}, spec);
    // Fixed controlled liquidity at the same price, not real market records.
    run.externalOrder(0, Side::Sell, spec.price("43000.12"), spec.quantity(partial ? "0.00050" : "0.00123"));
    if (partial) run.externalOrder(35, Side::Sell, spec.price("43000.12"), spec.quantity("0.00073"));
    std::ostringstream out;
    out << "ILLUSTRATIVE BTC/USDT-style settings; not Binance trading filters; no historical data\n";
    out << "mode=" << (scheduled ? "scheduled_5_slices" : partial ? "partial_fill" : "immediate")
        << " initial_cash=1000 USDT initial_inventory=0 BTC target=0.00123 BTC limit=43010 USDT/BTC\n";
    if (!scheduled) out << "initial_order_cash_reservation=" << spec.cashText(product(task.limit_price, task.target)) << " USDT\n";
    else {
        out << "scheduled_quantity_BTC=";
        const auto plan = slicePlan(task, 5, spec);
        for (std::size_t i = 0; i < plan.size(); ++i) { if (i) out << ','; out << spec.quantityText(plan[i].quantity) << '@' << plan[i].time; }
        out << "us\n";
    }
    out << run.run(); return out.str();
}
}
