#include "execution/decimal_demo.h"
#include "execution/comparison.h"
#include <iostream>
using namespace execution;
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
void rejects(const std::function<void()>& f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Expected rejection");}
Report fill(ull id,ull execution,Side side,ll price,ll quantity){return {ReportType::Fill,id,execution,side,price,quantity,5,10,true,""};}
void parsing() {
    check(parseDecimal("43000.12",2)==4300012 && parseDecimal("0.00123000",8)==123000,"Exact example price/quantity");
    check(parseDecimal("52.8901476",10)==528901476000LL,"Exact sub-cent notional");
    check(parseDecimal("1.230000",2)==123 && parseDecimal("7.0000",0)==7 && parseDecimal("000.01000",2)==1,"Trailing zeroes and leading zeroes accepted");
    check(parseDecimal("0",18)==0 && parseDecimal("0.000000000000000001",18)==1,"Zero and smallest atom");
    check(formatDecimal(528901476000LL,10)=="52.8901476" && formatDecimal(9471098524000LL,10)=="947.1098524","Cash doesn't truncate to cents");
    check(formatDecimal(0,0)=="0" && formatDecimal(0,18)=="0" && formatDecimal(1,10)=="0.0000000001","Zero and tiny cash formatting");
    for(const auto& text:{"","-1","+1",".1","1.","1..0","1e2","NaN","inf","Infinity"," 1","1 ","1,2","1.2301"})rejects([&]{parseDecimal(text,2);});
    const ll maximum=std::numeric_limits<ll>::max();check(parseDecimal("9223372036854775807",0)==maximum,"Maximum atoms");
    check(parseDecimal("9.223372036854775807000",18)==maximum,"Maximum scaled amount with removable zeros");
    rejects([]{parseDecimal("9223372036854775808",0);});rejects([]{parseDecimal("922337203685477580.8",1);});rejects([]{parseDecimal("1",19);});rejects([]{formatDecimal(-1,2);});
    for(const auto places:{0U,2U,8U,10U,18U})for(ll value:{0LL,1LL,123000LL,528901476000LL,maximum})check(parseDecimal(formatDecimal(value,places),places)==value,"Exact deterministic round trip");
}
void specificationsAndGrid() {
    Instrument spec=illustrativeBTC();check(spec.cashDecimals()==10 && spec.price_tick_atoms==1 && spec.quantity_step_atoms==1000,"Explicit scales");
    for(int change=0;change<8;++change){auto invalid=spec;
        if(change==0)invalid.price_decimals=19;
        if(change==1)invalid.quantity_decimals=17;
        if(change==2)invalid.price_tick_atoms=0;
        if(change==3)invalid.quantity_step_atoms=-1;
        if(change==4)invalid.symbol="";
        if(change==5)invalid.quote="US DT";
        if(change==6)invalid.max_price_atoms=0;
        if(change==7)invalid.quantity_step_atoms=invalid.max_quantity_atoms+1;
        rejects([&]{invalid.validate();});rejects([&]{Experiment e({Side::Buy,123000,0,10,4301000},{10000000000000LL,0},0,0,{},invalid);});
    }
    spec.price_tick_atoms=5;
    check(spec.price("43000.12")==4300012,"Parsing is separate from grid validation");rejects([&]{spec.validatePrice(4300012);});
    OrderManager manager({Side::Buy,123000,0,100,4301000},{10000000000000LL,0},spec);
    check(!manager.submit(1,Side::Buy,4301001,123000).accepted && !manager.submit(2,Side::Buy,4301000,123001).accepted,"Wrong tick/step local rejections");
    check(manager.account.cash==10000000000000LL && manager.reservedCash()==0,"Grid rejection leaves resources unchanged");
    std::vector<Report> reports;Exchange exchange([&](Report r){reports.push_back(r);},spec);
    exchange.submit({1,Side::Sell,4300012,123000},false,0);exchange.submit({2,Side::Sell,4300010,123001},false,0);
    check(reports.size()==2 && reports[0].type==ReportType::Rejected && reports[1].type==ReportType::Rejected && exchange.truth.empty(),"External grid validated before matching");
    exchange.submit({3,Side::Sell,4300010,123000},false,0);exchange.submit({4,Side::Buy,4301000,123000},true,1);check(exchange.truth.size()==1 && exchange.truth[0].price==4300010,"Rejected liquidity never enters engine");
    rejects([&]{Experiment bad({Side::Buy,123001,0,10,4301000},{10000000000000LL,0},0,0,{},spec);});
}
void handPurchaseAndTiming() {
    const auto spec=illustrativeBTC();Experiment run({Side::Buy,123000,10,100,4301000},{10000000000000LL,0},2,5,{},spec);
    run.externalOrder(0,Side::Sell,4300012,123000);
    while(run.exchangeExecutions().empty())check(run.step(),"No engine execution");
    check(run.local().account.cash==10000000000000LL && run.local().account.inventory==0 && run.local().reservedCash()==529023000000LL,"Undelivered decimal execution retains reservation/account");
    check(run.exchangeExecutions()[0].quantity==123000 && run.exchangeExecutions()[0].price==4300012,"Real engine decimal-atom execution");
    const auto summary=run.finish();check(summary.notional==528901476000LL && summary.cash==9471098524000LL && summary.inventory==123000 && summary.completed==123000,"Independent hand-calculated purchase");
    check(summary.reserved_cash==0 && summary.reserved_inventory==0 && summary.unresolved_orders==0,"Complete exact settlement");
    Experiment tiny({Side::Buy,1000,0,10,1},{10000000000000LL,0},0,0,{},spec);
    tiny.externalOrder(0,Side::Sell,1,1000);const auto tiny_summary=tiny.finish();
    check(tiny_summary.notional==1000 && tiny_summary.cash==9999999999000LL && spec.cashText(tiny_summary.cash)=="999.9999999","Engine-generated sub-cent settlement remains exact");
    const auto text=run.run();check(text.find("cash_paise")==std::string::npos && text.find("price_paise")==std::string::npos && text.find("cash_decimals=10")!=std::string::npos,"USDT atoms never labeled paise");
    check(text.find("notional=52.8901476 USDT")!=std::string::npos,"Exact result formatted");
}
void partialsReservationsAndDuplicates() {
    const auto spec=illustrativeBTC();OrderManager manager({Side::Buy,123000,0,100,4301000},{10000000000000LL,0},spec);
    check(manager.submit(1,Side::Buy,4301000,123000).accepted && manager.reservedCash()==529023000000LL,"Exact initial reservation");
    manager.requestCancel(1);check(manager.reservedCash()==529023000000LL,"Pending cancellation retains scaled cash");
    const auto report=fill(1,1,Side::Buy,4300012,50000);manager.receive(report);
    check(manager.account.cash==9784999400000LL && manager.account.inventory==50000 && manager.reservedCash()==313973000000LL && manager.availableCash()==9471026400000LL,"Independent partial fill, remainder and price improvement");
    check(!manager.receive(report) && manager.reservedCash()==313973000000LL && manager.executed==50000 && manager.account.cash==9784999400000LL,"Scaled duplicate does not debit/release twice");
    auto off_grid=fill(1,2,Side::Buy,4300012,1);rejects([&]{manager.receive(off_grid);});
    check(manager.reservedCash()==313973000000LL && manager.account.inventory==50000,"Invalid numerical fill atomicity");
    manager.receive({ReportType::Cancelled,1,0,Side::Buy,4301000,73000,6,11,true,""});
    check(manager.reservedCash()==0 && manager.account.cash==9784999400000LL && manager.order(1).cancelled==73000,"Authoritative cancellation remainder release");
    OrderManager rejected({Side::Buy,123000,0,100,4301000},{10000000000000LL,0},spec);rejected.submit(1,Side::Buy,4301000,123000);
    rejected.receive({ReportType::Rejected,1,0,Side::Buy,4301000,0,1,2,true,"controlled_rejection"});check(rejected.reservedCash()==0 && rejected.account.cash==10000000000000LL,"Scaled rejection release");
    const auto text=fractionalDemo(false,true);check(text.find("notional=52.89736 USDT")!=std::string::npos && text.find("cash=947.10264 USDT")!=std::string::npos,"Partial demonstration uses later resting limit price, not fabricated ask price");
}
void sellAndLots() {
    const auto spec=illustrativeBTC();OrderManager manager({Side::Sell,123000,0,100,4300000},{0,123000},spec);
    manager.submit(1,Side::Sell,4300000,123000);check(manager.reservedInventory()==123000 && manager.availableInventory()==0,"Decimal sell reserve");
    manager.requestCancel(1);manager.receive(fill(1,1,Side::Sell,4300012,50000));
    check(manager.account.cash==215000600000LL && manager.account.inventory==73000 && manager.reservedInventory()==73000 && manager.order(1).cancel_pending,"Sell partial proceeds and reservation");
    manager.receive(fill(1,2,Side::Sell,4300012,73000));check(manager.account.cash==528901476000LL && manager.account.inventory==0 && manager.reservedInventory()==0,"Exact final sell proceeds");
    manager.receive({ReportType::CancelRejected,1,0,Side::Sell,4300000,0,6,11,true,"already_filled"});check(!manager.outstanding(),"Complete sell cancel race settles");
    const Task task{Side::Buy,123000,10,100,4301000};const auto slices=slicePlan(task,5,spec);
    const ll amounts[]={25000,25000,25000,24000,24000};ll total=0;
    for(std::size_t i=0;i<slices.size();++i){check(slices[i].quantity==amounts[i] && slices[i].time==10+18*static_cast<ll>(i),"Uneven lot slice allocation");spec.validateQuantity(slices[i].quantity);total+=slices[i].quantity;}
    check(total==123000,"Exact atom target conserved");rejects([&]{slicePlan(task,124,spec);});
    Scenario scenario{"decimal",task,{10000000000000LL,0},{{0,ScriptKind::Submit,"ask",Side::Sell,4300012,123000}},spec};
    const auto result=compareRun(scenario,{PolicyKind::Scheduled,5});check(result.summary.submitted_orders==5 && result.summary.completed==123000 && result.summary.notional==528901476000LL && result.summary.cash==9471098524000LL,"Existing comparison/order manager reused for decimal slices");
    check(fractionalDemo(true).find("notional=52.8901476 USDT")!=std::string::npos,"Scheduled decimal demo notional");
}
void overflowAndTotals() {
    auto spec=illustrativeBTC();spec.max_price_atoms=std::numeric_limits<ll>::max();spec.max_quantity_atoms=std::numeric_limits<ll>::max();spec.quantity_step_atoms=1;spec.validate();
    OrderManager manager({Side::Buy,10000,0,100,1000000000000000LL},{std::numeric_limits<ll>::max(),0},spec);
    const auto rejected=manager.submit(1,Side::Buy,1000000000000000LL,10000);check(!rejected.accepted && manager.account.cash==std::numeric_limits<ll>::max() && manager.reservedCash()==0,"Notional overflow rejects before reservation");
    std::vector<Report> reports;Exchange exchange([&](Report r){reports.push_back(r);},spec);exchange.submit({1,Side::Buy,1000000000000000LL,10000},true,0);check(reports.size()==1 && reports[0].type==ReportType::Rejected && exchange.truth.empty(),"Adapter notional overflow report");
    OrderManager funds({Side::Buy,2,0,100,5000000000000000000LL},{std::numeric_limits<ll>::max(),0},spec);
    check(funds.submit(1,Side::Buy,5000000000000000000LL,1).accepted,"Large representable first reservation");
    check(!funds.submit(2,Side::Buy,5000000000000000000LL,1).accepted && funds.reservedCash()==5000000000000000000LL,"Combined reservations cannot exceed representable account");
    OrderManager inventory({Side::Buy,1,0,10,1},{1,std::numeric_limits<ll>::max()},spec);check(!inventory.submit(1,Side::Buy,1,1).accepted && inventory.reservedCash()==0,"Inventory-total overflow prevalidation");
    OrderManager proceeds({Side::Sell,1,0,10,1},{1,1},spec);check(!proceeds.submit(1,Side::Sell,1,1).accepted && proceeds.reservedInventory()==0,"Cash-total proceeds headroom checked before mutation");
    rejects([]{add(std::numeric_limits<ll>::max(),1);});rejects([]{product(std::numeric_limits<ll>::max(),2);});
}
void repeatabilityAndZero() {
    check(fractionalDemo()==fractionalDemo() && fractionalDemo(true)==fractionalDemo(true) && fractionalDemo(false,true)==fractionalDemo(false,true),"Deterministic decimal output");
    const auto spec=illustrativeBTC();Experiment empty({Side::Buy,1000,0,10,4300000},{10000000000000LL,0},0,0,{},spec);const auto text=empty.run();
    check(text.find("average_price_atoms=unavailable")!=std::string::npos && text.find("average_price=unavailable")!=std::string::npos,"Zero-fill average handling");
    check(immediateDemo().find("cash_paise=495")!=std::string::npos && raceDemo().find("cash_paise=700")!=std::string::npos,"Legacy results");
}
int main(){const std::vector<std::pair<std::string,std::function<void()>>> tests={
    {"strict exact decimals and meaningful cash formatting",parsing},
    {"specification validation and separate tick/step checks",specificationsAndGrid},
    {"hand purchase and delivered-account timing",handPurchaseAndTiming},
    {"partials, price improvement, cancellation and duplicates",partialsReservationsAndDuplicates},
    {"sell accounting and lot-based existing policies",sellAndLots},
    {"notional, reservation and total overflow atomicity",overflowAndTotals},
    {"deterministic demos, legacy values and zero-fill averages",repeatabilityAndZero}};
    for(const auto& t:tests)try{t.second();std::cout<<"PASS: "<<t.first<<'\n';}catch(const std::exception& e){std::cerr<<"FAIL: "<<t.first<<": "<<e.what()<<'\n';return 1;}}
