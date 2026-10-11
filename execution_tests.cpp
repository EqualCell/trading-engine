#include "execution/experiment.h"
#include <iostream>
using namespace execution;
void check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
void rejects(const std::function<void()>& action) { bool rejected = false; try { action(); } catch(const std::exception&) { rejected = true; } check(rejected, "Expected rejection"); }
void matchingAndDelivery() {
    Experiment run({Side::Buy, 5, 10, 100, 105}, {1000,0}, 5, 7);
    run.externalOrder(0,Side::Sell,103,2); run.externalOrder(0,Side::Sell,100,2); run.externalOrder(0,Side::Sell,101,2);
    while (run.exchangeExecutions().empty()) check(run.step(), "No exchange execution");
    check(run.local().account.cash==1000 && run.local().account.inventory==0, "Account changed before delivery");
    check(run.exchangeExecutions().size()==3, "Engine fill count");
    check(run.exchangeExecutions()[0].price==100 && run.exchangeExecutions()[0].quantity==2 && run.exchangeExecutions()[1].price==101 && run.exchangeExecutions()[1].quantity==2 && run.exchangeExecutions()[2].price==103 && run.exchangeExecutions()[2].quantity==1, "Best-price and partial fills");
    check(run.step() && run.local().order.acceptance_known && run.local().order.executed==0, "Acceptance is not a fill");
    const auto summary=run.finish(); check(summary.completed==5 && summary.cash==495 && summary.inventory==5 && summary.by_deadline==5 && summary.late==0, "Purchase accounting");
}
void laterExternalAndCancel() {
    Experiment run({Side::Buy,5,10,100,100},{1000,0},5,7);
    run.externalOrder(30,Side::Sell,99,2);
    auto s=run.finish();check(s.completed==2 && s.uncompleted==3 && s.cash==800 && s.inventory==2 && s.outcome==Outcome::Cancelled,"External fill and cancel remainder");
    check(run.local().order.cancelled==3 && !run.local().order.cancel_pending,"Cancelled quantity");
    const auto trace=run.eventTrace(); check(trace.find("quantity=3 strategy reason=none state=cancelled")!=std::string::npos,"Cancellation trace");
}
void races() {
    Experiment full({Side::Buy,3,10,30,100},{1000,0},10,25); full.externalOrder(35,Side::Sell,99,3);
    bool saw_pending_fill=false;
    while(full.step()) if(full.local().order.executed==3 && full.local().order.cancel_pending) saw_pending_fill=true;
    auto s=full.finish();check(saw_pending_fill && s.completed==3 && s.late==3 && s.by_deadline==0 && s.outcome==Outcome::Filled && s.cash==700,"Full fill wins race");
    check(!full.local().order.cancel_pending && full.local().order.acceptance_known,"Cancel rejection settles without reopening");
    Experiment partial({Side::Buy,5,10,30,100},{1000,0},10,25);partial.externalOrder(35,Side::Sell,99,2);
    s=partial.finish();check(s.completed==2 && s.late==2 && s.uncompleted==3 && s.outcome==Outcome::Cancelled,"Partial race then cancellation");
    check(partial.local().order.cancelled==3,"Partial race cancellation amount");
    // Later liquidity after successful cancellation cannot fill the order.
    Experiment cancelled({Side::Buy,3,10,30,100},{1000,0},10,5);cancelled.externalOrder(41,Side::Sell,99,3);
    s=cancelled.finish();check(s.completed==0 && s.cash==1000 && s.inventory==0 && s.outcome==Outcome::Cancelled,"Post-cancel no fill");
}
void deadlineBeforeAck() {
    Experiment run({Side::Buy,3,10,20,100},{1000,0},50,100);
    bool unknown_cancel=false;
    while(run.step()) if(run.local().order.cancel_pending && !run.local().order.acceptance_known) unknown_cancel=true;
    auto s=run.finish();check(unknown_cancel && s.completed==0 && s.outcome==Outcome::Cancelled && run.local().order.cancelled==3,"Deadline before submit arrival/ack");
    auto trace=run.eventTrace(); check(trace.find("type=submit_arrival")<trace.find("type=cancel_arrival"),"Request order preserved");
    Experiment delayedReport({Side::Buy,2,10,20,100},{1000,0},0,100);delayedReport.externalOrder(0,Side::Sell,99,2);
    s=delayedReport.finish();check(s.by_deadline==2 && s.late==0 && s.completed==2 && s.cash==802,"Execution time, not report time");
    // Exactly at the deadline is included, although the delivered deadline timer
    // can request cancellation before its fill report is delivered.
    Experiment equal({Side::Buy,2,0,10,100},{1000,0},0,0);equal.externalOrder(10,Side::Sell,99,2);
    s=equal.finish();check(s.by_deadline==2 && s.late==0 && s.outcome==Outcome::Filled,"Inclusive deadline timestamp");
}
void validationAndRejections() {
    Experiment cash({Side::Buy,2,0,10,100},{199,0});auto s=cash.finish();check(s.outcome==Outcome::Rejected && s.completed==0 && s.cash==199 && cash.exchangeExecutions().empty(),"Buy buying power rejection");
    Experiment inventory({Side::Sell,2,0,10,100},{100,1});s=inventory.finish();check(s.outcome==Outcome::Rejected && s.inventory==1 && s.cash==100,"No short selling");
    Experiment buyOverflow({Side::Buy,1,0,10,1},{10,std::numeric_limits<ll>::max()});s=buyOverflow.finish();check(s.outcome==Outcome::Rejected && s.inventory==std::numeric_limits<ll>::max(),"Inventory overflow prevalidation");
    Experiment sellOverflow({Side::Sell,1,0,10,1},{std::numeric_limits<ll>::max(),1});check(sellOverflow.finish().outcome==Outcome::Rejected,"Cash overflow prevalidation");
    std::vector<Report> reports;Exchange adapter([&](Report r){reports.push_back(r);});
    adapter.submit({1,Side::Buy,0,2},true,0);check(reports.size()==1 && reports[0].type==ReportType::Rejected && adapter.truth.empty(),"Engine validation becomes report");
    adapter.cancel(1,true,1);check(reports.back().type==ReportType::CancelRejected && !reports.back().reason.empty(),"Rejected order cancellation reason");
    adapter.cancel(999,false,2);check(reports.back().reason=="unknown_order","Unknown cancellation reason");
    adapter.submit({2,Side::Sell,100,1},false,3);adapter.submit({3,Side::Buy,100,1},true,4);
    check(adapter.truth.size()==1 && adapter.truth[0].quantity==1,"Rejection didn't corrupt adapter");
    adapter.submit({2,Side::Sell,100,1},false,5);check(reports.back().type==ReportType::Rejected,"IDs not reused");
    Experiment sell({Side::Sell,3,5,20,100},{100,3});sell.externalOrder(0,Side::Buy,102,2);s=sell.finish();check(s.completed==2 && s.uncompleted==1 && s.cash==304 && s.inventory==1 && s.outcome==Outcome::Cancelled,"Sell actual prices/accounting");
    Experiment restingSell({Side::Sell,2,5,20,100},{100,2},2,3);
    restingSell.externalOrder(10,Side::Buy,105,2);s=restingSell.finish();
    check(s.completed==2 && s.cash==300 && s.inventory==0 && s.by_deadline==2,"External buy fills resting strategy sell at resting price");
    OrderManager rejected(1,{Side::Buy,2,0,10,0},{100,0});rejected.submitted();
    Exchange rejectionDelivery([&](Report report){report.delivery_time=report.exchange_time;rejected.receive(report);});
    rejectionDelivery.submit({1,Side::Buy,0,2},true,0);
    check(rejected.order.outcome==Outcome::Rejected && rejected.account.cash==100 && rejected.account.inventory==0,"Delivered exchange rejection leaves balances unchanged");
    Experiment externalCancelled({Side::Buy,1,10,20,100},{1000,0});auto id=externalCancelled.externalOrder(0,Side::Sell,100,1);externalCancelled.externalCancel(5,id);check(externalCancelled.finish().completed==0,"Scripted liquidity cancellation");
}
void duplicatesAndInvariants() {
    const Task task{Side::Buy,3,0,10,100};OrderManager manager(1,task,{1000,0});manager.submitted();manager.requestCancel();
    Report fill{ReportType::Fill,1,42,Side::Buy,99,2,5,10,true,""};check(manager.receive(fill),"First delivery");
    fill.delivery_time=11;check(!manager.receive(fill) && manager.account.cash==802 && manager.account.inventory==2 && manager.order.executed==2 && manager.order.remaining==1 && manager.order.cancel_pending,"Duplicate not counted");
    auto conflicting=fill;conflicting.quantity=1;rejects([&]{manager.receive(conflicting);});check(manager.account.cash==802,"Conflict unchanged");
    auto overfill=fill;overfill.execution_id=43;rejects([&]{manager.receive(overfill);});
    Report cancel{ReportType::Cancelled,1,0,Side::Buy,100,1,6,12,true,""};manager.receive(cancel);
    auto after=fill;after.execution_id=44;after.quantity=1;rejects([&]{manager.receive(after);});
    check(!manager.receive(fill) && manager.account.cash==802 && manager.order.cancelled==1 && manager.order.outcome==Outcome::Cancelled,"Terminal duplicate does not reopen order");
}
void schedulerAndTime() {
    Scheduler scheduler;std::vector<int> ordering;
    scheduler.schedule(0,[&](Time time,ull seq){check(time==0 && seq==1,"Sequence");ordering.push_back(1);scheduler.schedule(0,[&](Time,ull){ordering.push_back(3);});});
    scheduler.schedule(0,[&](Time,ull){ordering.push_back(2);});scheduler.drain();check(ordering==std::vector<int>({1,2,3}),"Same-time child ordered after queued peers");
    scheduler.schedule(5,[](Time,ull){});scheduler.drain();rejects([&]{scheduler.schedule(4,[](Time,ull){});});rejects([&]{scheduler.schedule(-1,[](Time,ull){});});
    Scheduler exhausted(std::numeric_limits<ull>::max());exhausted.schedule(0,[](Time,ull){});rejects([&]{exhausted.schedule(0,[](Time,ull){});});
    rejects([]{delayed(std::numeric_limits<ll>::max(),1);});rejects([]{product(std::numeric_limits<ll>::max(),2);});rejects([]{add(std::numeric_limits<ll>::min(),-1);});
    rejects([]{Experiment invalid({Side::Buy,0,0,10,1},{100,0});});
    rejects([]{Experiment invalid({static_cast<Side>(42),1,0,10,1},{100,0});});
    rejects([]{Experiment invalid({Side::Buy,1,0,10,0},{100,0});});
    rejects([]{Experiment invalid({Side::Buy,1,0,10,1},{-1,0});});
    rejects([]{Experiment invalid({Side::Buy,1,10,10,1},{100,0});});rejects([]{Experiment invalid({Side::Buy,1,-1,10,1},{100,0});});rejects([]{Experiment invalid({Side::Buy,1,0,10,1},{100,0},-1,0);});rejects([]{Experiment invalid({Side::Buy,1,0,10,1},{100,0},0,-1);});
    rejects([]{Experiment invalid({Side::Buy,1,0,std::numeric_limits<ll>::max(),1},{100,0},1,0);});
    Experiment valid({Side::Buy,1,0,10,1},{100,0},0,1);rejects([&]{valid.externalOrder(std::numeric_limits<ll>::max(),Side::Sell,1,1);});
}
int main() {
    const std::vector<std::pair<std::string,std::function<void()>>> tests={
        {"best prices, partial fills, report-only accounting",matchingAndDelivery},
        {"external initiation and cancelled remainder",laterExternalAndCancel},
        {"complete/partial cancellation races",races},
        {"deadline before ack and occurrence-time classification",deadlineBeforeAck},
        {"local/exchange validation, sell accounting, external cancellation",validationAndRejections},
        {"duplicate/conflicting fills and terminal invariants",duplicatesAndInvariants},
        {"scheduler/time/overflow",schedulerAndTime},
        {"deterministic traces",[]{
            check(immediateDemo()==immediateDemo() && raceDemo()==raceDemo(),"Repeated trace bytes differ");
            Experiment empty({Side::Buy,1,0,10,100},{1000,0});check(empty.run().find("average_price_paise=unavailable")!=std::string::npos,"Zero-fill price display");
            Experiment fractional({Side::Buy,2,1,10,101},{1000,0});fractional.externalOrder(0,Side::Sell,100,1);fractional.externalOrder(0,Side::Sell,101,1);
            check(fractional.run().find("average_price_paise=100 (floor display; exact ratio 201/2)")!=std::string::npos,"Fractional average display is explicitly rounded");
        }}
    };
    for(const auto& test:tests) try{test.second();std::cout<<"PASS: "<<test.first<<'\n';}catch(const std::exception& error){std::cerr<<"FAIL: "<<test.first<<": "<<error.what()<<'\n';return 1;}
}
