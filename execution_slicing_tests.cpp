#include "execution/comparison.h"
#include <iostream>
using namespace execution;
void check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
void rejects(const std::function<void()>& action) { bool failed=false;try{action();}catch(const std::exception&){failed=true;}check(failed,"Expected rejection"); }
Report fill(ull order, ull execution, Side side, ll price, ll quantity, Time time=1) { return {ReportType::Fill,order,execution,side,price,quantity,time,time+10,true,""}; }
Report cancel(ull order, Side side, ll price, ll quantity) { return {ReportType::Cancelled,order,0,side,price,quantity,2,12,true,""}; }
void schedules() {
    auto plan=slicePlan({Side::Buy,10,0,100000,100},5);
    for(std::size_t i=0;i<plan.size();++i) check(plan[i].time==static_cast<ll>(i)*20000 && plan[i].quantity==2,"Exact five-slice example");
    plan=slicePlan({Side::Buy,10,5,22,100},3);
    check(plan[0].quantity==4 && plan[1].quantity==3 && plan[2].quantity==3 && plan[0].time==5 && plan[1].time==10 && plan[2].time==16,"Nondivisible quantities and floor timestamps");
    ll total=0;for(const auto& slice:plan)total+=slice.quantity;check(total==10,"Slice target sum");
    for(ll count: {0LL,-1LL,11LL}) rejects([&]{slicePlan({Side::Buy,10,0,100,100},count);});
    rejects([]{slicePlan({Side::Buy,10,0,2,100},3);});rejects([]{slicePlan({Side::Buy,10,4,4,100},1);});
    const ll maximum=std::numeric_limits<ll>::max();plan=slicePlan({Side::Buy,4,0,maximum,100},4);
    check(plan[1].time==maximum/4 && plan[2].time==maximum/2 && plan[3].time==maximum-maximum/4-1,"Overflow-safe floor i*duration/N");
    plan=slicePlan({Side::Buy,3,maximum-3,maximum,100},3);check(plan[2].time==maximum-1,"Large start time");
}
void buyReservations() {
    OrderManager manager({Side::Buy,6,0,100,100},{1000,0});
    check(manager.submit(1,Side::Buy,100,2).accepted && manager.submit(2,Side::Buy,100,3).accepted,"Multiple unacknowledged submissions");
    check(manager.reservedCash()==500 && manager.availableCash()==500 && manager.account.cash==1000 && !manager.order(1).acceptance_known && manager.potentiallyExecutable()==5,"Pre-ack reservation/exposure");
    manager.requestCancel(1);check(manager.reservedCash()==500,"Pending cancel retains cash");
    const auto report=fill(1,1,Side::Buy,90,1);manager.receive(report);
    check(manager.account.cash==910 && manager.account.inventory==1 && manager.reservedCash()==400 && manager.availableCash()==510 && manager.order(1).reserved_cash==100,"Partial release and price improvement");
    auto duplicate=report;duplicate.delivery_time=12;check(!manager.receive(duplicate) && manager.reservedCash()==400 && manager.account.cash==910 && manager.executed==1,"Duplicate reservation/account idempotence");
    duplicate.quantity=2;rejects([&]{manager.receive(duplicate);});check(manager.reservedCash()==400,"Conflicting duplicate unchanged");
    auto cross_order=report;cross_order.order_id=2;rejects([&]{manager.receive(cross_order);});
    check(manager.order(2).executed==0 && manager.reservedCash()==400,"Conflicting cross-order execution ID preserves reservations");
    manager.receive(cancel(1,Side::Buy,100,1));check(manager.reservedCash()==300 && manager.order(2).reserved_cash==300 && manager.successful_cancellations==1,"Cancellation only releases own remainder");
    manager.receive({ReportType::Rejected,2,0,Side::Buy,100,0,2,12,true,"venue_validation"});
    check(manager.reservedCash()==0 && manager.account.cash==910 && manager.account.inventory==1 && manager.exchange_rejections==1,"Rejection releases own order");
    manager.receive({ReportType::Rejected,2,0,Side::Buy,100,0,2,13,true,"venue_validation"});check(manager.exchange_rejections==1,"Duplicate rejection doesn't release again");
}
void submissionBoundaries() {
    OrderManager cash({Side::Buy,5,0,100,100},{150,0});check(cash.submit(1,Side::Buy,100,1).accepted,"First funded order");
    check(!cash.submit(2,Side::Buy,100,1).accepted && cash.availableCash()==50 && cash.reservedCash()==100 && cash.account.cash==150 && cash.submitted_quantity==1,"Overspending rejected locally");
    OrderManager inventory({Side::Sell,5,0,100,100},{0,2});check(inventory.submit(1,Side::Sell,100,2).accepted,"First inventory reservation");
    check(!inventory.submit(2,Side::Sell,100,1).accepted && inventory.reservedInventory()==2 && inventory.availableInventory()==0,"Overselling rejected");
    OrderManager target({Side::Buy,3,0,100,100},{1000,0});check(target.submit(1,Side::Buy,100,2).accepted,"Target setup");
    target.requestCancel(1);check(!target.submit(2,Side::Buy,100,2).accepted && target.potentiallyExecutable()==2,"Pending cancellation counts against target");
    check(!target.submit(3,Side::Buy,101,1).accepted,"No repricing beyond task limit");
    rejects([&]{target.submit(4,Side::Sell,100,1);});rejects([&]{target.submit(4,Side::Buy,100,0);});rejects([&]{target.submit(1,Side::Buy,100,1);});
    check(target.reservedCash()==200 && target.executed==0,"Invalid requests preserve resources");
    target.receive(fill(1,1,Side::Buy,100,1));check(!target.submit(5,Side::Buy,100,2).accepted,"Executed plus unacknowledged remainder exposure");
    OrderManager improvement({Side::Buy,3,0,100,100},{250,0});improvement.submit(1,Side::Buy,100,2);
    check(!improvement.submit(2,Side::Buy,100,1).accepted,"Before improvement insufficient available cash");
    improvement.receive(fill(1,10,Side::Buy,40,1));
    check(improvement.availableCash()==110 && improvement.submit(3,Side::Buy,100,1).accepted && improvement.reservedCash()==200 && improvement.account.cash==210,"Delivered price improvement funds next order without freeing resting remainder");
    OrderManager bound({Side::Buy,2,0,100,100},{1000,0});check(!bound.submit(1,Side::Buy,std::numeric_limits<ll>::max(),1).accepted && bound.reservedCash()==0,"Unsupported price rejected without overflow/mutation");
}
void sellReservationsAndFailedCancel() {
    OrderManager sell({Side::Sell,4,0,100,100},{100,4});sell.submit(1,Side::Sell,100,2);sell.submit(2,Side::Sell,100,2);
    check(sell.reservedInventory()==4 && sell.availableInventory()==0,"Sell pre-ack reserves");
    sell.requestCancel(1);sell.receive(fill(1,1,Side::Sell,110,1));check(sell.account.cash==210 && sell.account.inventory==3 && sell.reservedInventory()==3 && sell.order(1).cancel_pending,"Sell partial during pending cancellation");
    sell.receive({ReportType::CancelRejected,1,0,Side::Sell,100,0,2,12,true,"still_active"});
    check(sell.reservedInventory()==3 && sell.order(1).remaining==1 && sell.outstanding(1),"Failed cancel doesn't release/reopen state");
    sell.requestCancel(2);sell.receive(cancel(2,Side::Sell,100,2));check(sell.reservedInventory()==1 && sell.availableInventory()==2,"Own sell remainder release");
    sell.receive(fill(1,2,Side::Sell,105,1));check(sell.reservedInventory()==0 && sell.account.inventory==2 && sell.account.cash==315 && sell.executed==2,"Sell complete accounting");
}
void severalOrdersAndDeadline() {
    Experiment run({Side::Buy,6,0,60,100},{1000,0},30,100,{PolicyKind::Scheduled,3});
    run.externalOrder(75,Side::Sell,99,3); // After deadline request, before cancels arrive at 90.
    bool multiple_unknown=false, pending_fill=false, hidden_fill=false;
    while(run.step()) {
        const auto& manager=run.local();
        if(manager.orders().size()==3 && !manager.order(1).acceptance_known && manager.reservedCash()==600)multiple_unknown=true;
        if(manager.executed && manager.order(1).cancel_pending)pending_fill=true;
        if(!run.exchangeExecutions().empty() && manager.executed==0) {
            hidden_fill=true;check(manager.account.cash==1000 && manager.reservedCash()==600,"Undelivered fills/cancels cannot free account/reservations");
        }
        check(manager.executed+manager.potentiallyExecutable()<=6,"Task exposure during each delivery");
    }
    auto s=run.finish();check(multiple_unknown && pending_fill && hidden_fill && s.by_deadline==0 && s.late==3 && s.completed==3 && s.uncompleted==3,"Multi-order deadline/cancellation race");
    check(s.submitted_orders==3 && s.submitted_quantity==6 && s.successful_cancellations==2 && s.unresolved_orders==0 && s.reserved_cash==0 && s.cash==700 && s.inventory==3,"Several race responses settle");
    check(run.exchangeExecutions().size()==2 && run.exchangeExecutions()[0].quantity==2 && run.exchangeExecutions()[1].quantity==1,"One external order fills multiple strategy orders");
    // All slices execute early, even though their reports arrive after deadline.
    Experiment early({Side::Buy,6,0,60,100},{1000,0},0,100,{PolicyKind::Scheduled,3});early.externalOrder(0,Side::Sell,90,6);
    s=early.finish();check(s.by_deadline==6 && s.late==0 && s.completed==6 && s.cash==460 && s.reserved_cash==0,"Exchange-time on-time classification");
    Experiment empty({Side::Buy,6,0,60,100},{1000,0},0,0,{PolicyKind::Scheduled,3});s=empty.finish();
    check(s.completed==0 && s.uncompleted==6 && s.successful_cancellations==3 && s.reserved_cash==0 && s.unresolved_orders==0 && s.cash==1000,"No-fill settled summary");
    Experiment sell({Side::Sell,6,0,60,100},{100,6},0,5,{PolicyKind::Scheduled,3});
    sell.externalOrder(0,Side::Buy,110,2);sell.externalOrder(35,Side::Buy,105,3);
    s=sell.finish();check(s.completed==5 && s.uncompleted==1 && s.notional==525 && s.cash==625 && s.inventory==1 && s.reserved_inventory==0 && s.successful_cancellations==1,"Scheduled sell routing, actual proceeds and reservation settlement");
    check(empty.run().find("average_price_paise=unavailable")!=std::string::npos,"No fill average N/A");
}
void equalTimeAndNoRetries() {
    Experiment run({Side::Buy,3,0,3,100},{300,0},0,0,{PolicyKind::Scheduled,3});run.externalOrder(1,Side::Sell,90,1);
    auto s=run.finish();check(s.completed==1 && s.notional==100 && s.submitted_orders==3 && s.submitted_quantity==3 && s.successful_cancellations==2,"Queued external event fills old resting limit before same-time slice");
    const auto trace=run.eventTrace();check(trace.find("type=external_submit")<trace.find("type=slice_timer"),"Common external tie precedence");
    Experiment rejected({Side::Buy,3,0,30,100},{299,0},0,0,{PolicyKind::Scheduled,3});s=rejected.finish();
    check(s.submitted_orders==0 && s.local_rejections==3 && s.completed==0 && s.uncompleted==3 && s.cash==299,"Failed slices recorded, no retry/catchup");
    check(rejected.local().orders().size()==3,"Each failed attempt has order record");
}
void comparisons() {
    const auto scenarios=comparisonScenarios();
    const std::vector<ll> immediateCosts{540,660,420},scheduledCosts{600,600,400};
    for(std::size_t i=0;i<scenarios.size();++i) {
        const auto immediate=compareRun(scenarios[i],{}), scheduled=compareRun(scenarios[i],{PolicyKind::Scheduled,3});
        for(const auto* result:{&immediate,&scheduled}) {
            const auto& s=result->summary;check(s.target==6 && s.local_rejections==0 && s.exchange_rejections==0 && s.unresolved_orders==0 && s.reserved_cash==0 && s.reserved_inventory==0,"Fresh settled comparison");
            check(s.completed==(i==2?4:6) && s.by_deadline==(i==2?3:6) && s.late==(i==2?1:0) && s.uncompleted==(i==2?2:0),"Completion/classification hand calculation");
        }
        check(immediate.summary.notional==immediateCosts[i] && scheduled.summary.notional==scheduledCosts[i],"Hand-calculated execution costs");
        check(immediate.summary.cash==1000-immediateCosts[i] && scheduled.summary.cash==1000-scheduledCosts[i],"Independent account balances");
        check(immediate.summary.submitted_orders==1 && scheduled.summary.submitted_orders==3 && scheduled.summary.submitted_quantity==6,"Policy order counts");
        const auto repeated=compareRun(scenarios[i],{PolicyKind::Scheduled,3});check(repeated.trace==scheduled.trace && repeated.summary.notional==scheduled.summary.notional,"Fresh repeat identical");
    }
    const auto rising=compareRun(scenarios[0],{});check(rising.trace.find("not_resting: filled, rejected, or already_cancelled")!=std::string::npos,"Filled external cancellation stays in fixed script");
    check(comparisonText()==comparisonText() && comparisonText(3,true)==comparisonText(3,true),"Byte-identical tables and traces");
    // N=1 is the same policy timing/quantity as immediate, apart from labels.
    for(const auto& scenario:scenarios)check(compareRun(scenario,{}).trace==compareRun(scenario,{PolicyKind::Scheduled,1}).trace,"One slice equals immediate mechanics");
}
void overflow() {
    const ll maximum=std::numeric_limits<ll>::max();
    OrderManager inventory({Side::Buy,2,0,10,1},{10,maximum});check(!inventory.submit(1,Side::Buy,1,1).accepted && inventory.reservedCash()==0,"Inventory representability rejects before reservation");
    OrderManager proceeds({Side::Sell,2,0,10,1},{maximum,2});check(!proceeds.submit(1,Side::Sell,1,1).accepted && proceeds.reservedInventory()==0,"Proceeds headroom rejection");
    rejects([&]{Experiment bad({Side::Buy,2,0,maximum,1},{10,0},1,0,{PolicyKind::Scheduled,2});});
    rejects([]{Experiment bad({Side::Buy,2,0,10,1},{10,0},0,0,{PolicyKind::Scheduled,3});});
    rejects([]{comparisonText(0);});rejects([]{comparisonText(7);});
}
int main() {
    const std::vector<std::pair<std::string,std::function<void()>>> tests={
        {"slice allocation and overflow-safe exact timestamps",schedules},
        {"buy reservations, price improvement, own remainder, duplicates",buyReservations},
        {"local cash/inventory/target submission boundaries",submissionBoundaries},
        {"sell reservations and failed-cancel retention",sellReservationsAndFailedCancel},
        {"unacknowledged orders, multi-order races and deadline classification",severalOrdersAndDeadline},
        {"equal-time ordering, no fills and rejected slices without retry",equalTimeAndNoRetries},
        {"hand-calculated independent comparisons and repeatable output",comparisons},
        {"configuration and arithmetic boundaries",overflow}};
    for(const auto& test:tests)try{test.second();std::cout<<"PASS: "<<test.first<<'\n';}catch(const std::exception& error){std::cerr<<"FAIL: "<<test.first<<": "<<error.what()<<'\n';return 1;}
}
