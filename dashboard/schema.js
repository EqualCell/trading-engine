const I64_MIN = -(1n << 63n), I64_MAX = (1n << 63n) - 1n;
const U64_MAX = (1n << 64n) - 1n, SAFE = BigInt(Number.MAX_SAFE_INTEGER);

function assert(condition, message) {
  if (!condition) throw new Error(message);
}
function object(value, name) {
  assert(value !== null && typeof value === 'object' && !Array.isArray(value), `${name} must be an object.`);
}
function text(value, name) {
  assert(typeof value === 'string' && value.length > 0 && value.length <= 2000, `${name} must be readable text (1–2000 characters).`);
}
function integer(value, name, min = I64_MIN, max = I64_MAX) {
  assert(typeof value === 'string' && /^-?(0|[1-9][0-9]*)$/.test(value) && value.length <= 21,
    `${name} must be an integer encoded as a decimal string.`);
  const number = BigInt(value);
  assert(number >= min && number <= max, `${name} is outside its supported integer range.`);
  return number;
}
function positive(value, name) { return integer(value, name, 1n, I64_MAX); }
function id(value, name) { return integer(value, name, 1n, U64_MAX); }
function quote(value, name) {
  if (value === null) return;
  object(value, name);
  id(value.order_id, `${name}.order_id`);
  positive(value.price_paise, `${name}.price_paise`);
  positive(value.remaining_quantity, `${name}.remaining_quantity`);
}

// Preserve raw strings. Only exact, checked relative offsets reach Number plots.
export function safeOffset(value, origin) {
  const delta = BigInt(value) - BigInt(origin);
  assert(delta >= -SAFE && delta <= SAFE, 'Relative plotting range exceeds JavaScript exact integer precision.');
  return Number(delta);
}
export function money(value, signed = false) {
  let amount = BigInt(value);
  const sign = amount < 0n ? '−' : signed && amount > 0n ? '+' : '';
  if (amount < 0n) amount = -amount;
  return `${sign}₹${(amount / 100n).toLocaleString('en-IN')}.${(amount % 100n).toString().padStart(2, '0')}`;
}
export function units(value) { return BigInt(value).toLocaleString('en-IN'); }

export function validateScenario(data) {
  object(data, 'Export');
  assert(data.schema_version === 1, 'Unsupported schema version. Expected schema_version: 1.');
  text(data.scenario, 'Scenario'); text(data.description, 'Description');
  object(data.units, 'Units');
  assert(data.units.money === 'paise' && data.units.quantity === 'units' && data.units.time === 'microseconds', 'Expected paise, units, and microseconds.');
  assert(Array.isArray(data.assumptions) && data.assumptions.length > 0 && data.assumptions.length <= 50, 'Modeling assumptions are required.');
  data.assumptions.forEach((entry) => text(entry, 'Assumption'));
  object(data.strategy, 'Strategy');
  const maker = id(data.strategy.trader_id, 'Strategy trader ID');
  positive(data.strategy.half_spread_paise, 'Half spread');
  const limit = integer(data.strategy.inventory_limit, 'Inventory limit', 0n);
  assert(positive(data.strategy.order_quantity, 'Order quantity') === 1n, 'This dashboard schema uses one-unit strategy orders.');
  assert(Array.isArray(data.initial_accounts) && data.initial_accounts.length >= 2 && data.initial_accounts.length <= 100, 'Initial accounts are required.');
  const accountIds = new Set();
  let makerAccount;
  data.initial_accounts.forEach((account) => {
    object(account, 'Initial account');
    const trader = id(account.trader_id, 'Initial trader ID').toString();
    assert(!accountIds.has(trader), 'Duplicate initial account ID.'); accountIds.add(trader);
    integer(account.cash_paise, 'Initial cash'); integer(account.inventory, 'Initial inventory');
    if (BigInt(trader) === maker) makerAccount = account;
  });
  assert(makerAccount, 'Initial maker account is missing.');
  const initialValue = integer(data.initial_portfolio_value_paise, 'Initial portfolio value');
  assert(Array.isArray(data.events) && data.events.length > 0 && data.events.length <= 10000, 'Expected 1–10000 event snapshots.');
  let previousIndex = -1n, previousTime = -1n, totalTrades = 0;
  const origins = { inventory: data.events[0].inventory, total_pnl_paise: data.events[0].total_pnl_paise, price: data.events[0].reference_price_paise };
  data.events.forEach((event, position) => {
    object(event, `Event ${position}`);
    const index = integer(event.event_index, 'Event index', 0n, U64_MAX);
    const time = integer(event.timestamp_us, 'Timestamp', 0n, U64_MAX);
    assert(index > previousIndex && time >= previousTime, 'Event indexes must increase and timestamps must not go backward.');
    previousIndex = index; previousTime = time;
    text(event.type, 'Event type'); text(event.description, 'Event description');
    const reference = positive(event.reference_price_paise, 'Reference price');
    const cash = integer(event.cash_paise, 'Cash'), inventory = integer(event.inventory, 'Inventory');
    const value = integer(event.portfolio_value_paise, 'Marked value'), pnl = integer(event.total_pnl_paise, 'P&L');
    assert(inventory >= -limit && inventory <= limit, 'Snapshot exceeds strategy inventory limit.');
    assert(value === cash + inventory * reference && pnl === value - initialValue, 'Snapshot valuation does not match exact cash, inventory, and reference price.');
    quote(event.maker_bid, 'Maker bid'); quote(event.maker_ask, 'Maker ask');
    assert(!event.maker_bid || BigInt(event.maker_bid.remaining_quantity) === 1n, 'Maker bid must be one unit.');
    assert(!event.maker_ask || BigInt(event.maker_ask.remaining_quantity) === 1n, 'Maker ask must be one unit.');
    assert(!event.maker_bid || inventory < limit, 'Buy quote posted at long inventory limit.');
    assert(!event.maker_ask || inventory > -limit, 'Sell quote posted at short inventory limit.');
    assert(Array.isArray(event.trades) && event.trades.length <= 1000, 'Event trades must be an array of at most 1000 fills.');
    totalTrades += event.trades.length;
    assert(totalTrades <= 20000, 'Export exceeds the 20000-fill inspection limit.');
    event.trades.forEach((trade) => {
      object(trade, 'Trade'); id(trade.buy_id, 'Buy ID'); id(trade.sell_id, 'Sell ID');
      positive(trade.price_paise, 'Trade price'); positive(trade.quantity, 'Trade quantity');
    });
    safeOffset(event.timestamp_us, data.events[0].timestamp_us);
    safeOffset(event.reference_price_paise, origins.price);
    for (const side of ['maker_bid', 'maker_ask']) if (event[side]) safeOffset(event[side].price_paise, origins.price);
    for (const field of ['inventory', 'total_pnl_paise']) safeOffset(event[field], origins[field]);
  });
  return data;
}
