import { validateScenario, money, units, safeOffset } from './schema.js';

const $ = (id) => document.getElementById(id);
const colors = { reference: '#182c29', bid: '#126750', ask: '#3864a5', inventory: '#126750', pnl: '#ad4637' };
const svgNS = 'http://www.w3.org/2000/svg';
let scenario = null, importedScenario = null, position = 0, timer = null, request = 0, loadedOption = 'inventory-risk';

function svgNode(tag, attributes = {}, text) {
  const node = document.createElementNS(svgNS, tag);
  for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, value);
  if (text !== undefined) node.textContent = text;
  return node;
}
function pause() {
  clearInterval(timer); timer = null;
  $('play').textContent = '▶ Play'; $('play').setAttribute('aria-label', 'Play replay');
}
function controls(disabled = false) {
  for (const id of ['restart', 'previous', 'play', 'next', 'timeline']) $(id).disabled = disabled || !scenario;
  if (!disabled && scenario) {
    $('previous').disabled = position === 0;
    $('next').disabled = position === scenario.events.length - 1;
  }
}
function setPosition(next) {
  position = Math.max(0, Math.min(scenario.events.length - 1, next));
  render();
  if (position === scenario.events.length - 1) pause();
}
function play() {
  if (!scenario) return;
  if (timer) { pause(); return; }
  if (position === scenario.events.length - 1) setPosition(0);
  $('play').textContent = 'Ⅱ Pause'; $('play').setAttribute('aria-label', 'Pause replay');
  timer = setInterval(() => setPosition(position + 1), 1000 / Number($('speed').value));
}
function applyData(data) {
  scenario = validateScenario(data); position = 0;
  $('timeline').max = String(scenario.events.length - 1);
  $('description').textContent = scenario.description;
  $('error').hidden = true;
  $('metadata').textContent = `Schema v${scenario.schema_version} · ${scenario.scenario} · Maker trader ${scenario.strategy.trader_id} · Half-spread ${money(scenario.strategy.half_spread_paise)} · One-unit quotes`;
  $('assumptions').replaceChildren(...scenario.assumptions.map((assumption) => {
    const item = document.createElement('li'); item.textContent = assumption; return item;
  }));
  render();
}
function reportError(error, prefix) {
  $('error').textContent = `${prefix}: ${error.message}`; $('error').hidden = false;
  $('status').textContent = scenario ? 'Previous valid scenario remains available.' : 'No scenario loaded. Serve dashboard/ over HTTP, or import a schema v1 JSON file.';
  controls();
}
async function loadBundled(name) {
  const token = ++request;
  pause(); controls(true); $('error').hidden = true;
  $('status').textContent = 'Loading engine export…';
  try {
    const response = await fetch(`data/${name}.json`);
    if (!response.ok) throw new Error(`HTTP ${response.status}; scenario file could not be loaded.`);
    const text = await response.text();
    if (text.length > 10 * 1024 * 1024) throw new Error('Export exceeds the 10 MB limit.');
    const data = JSON.parse(text);
    if (token !== request) return;
    applyData(data); loadedOption = name;
  } catch (error) {
    if (token !== request) return;
    $('scenario').value = loadedOption;
    reportError(error, 'Could not load scenario');
  }
}

// Plot checked relative offsets; raw integers and all visible values stay BigInt.
// Equal timestamps share x coordinates. Event indexes in the log/slider preserve
// their order. A null quote ends its step path; the next quote starts a new one.
function drawChart(container, events, series, format, extraBounds = []) {
  const width = Math.max(280, container.clientWidth), height = container.classList.contains('small') ? 175 : 230;
  const left = 95, right = 20, top = 15, bottom = 35;
  const svg = svgNode('svg', { viewBox: `0 0 ${width} ${height}`, 'aria-hidden': 'true' });
  const allValues = series.flatMap((line) => events.map(line.value).filter((value) => value !== null));
  if (!allValues.length) {
    svg.append(svgNode('text', { x: width / 2, y: height / 2, 'text-anchor': 'middle', fill: '#576863' }, 'No values available yet'));
    container.replaceChildren(svg); return;
  }
  const origin = BigInt(allValues[0]);
  const offsets = [...allValues, ...extraBounds].map((value) => safeOffset(value, origin));
  let low = Math.min(...offsets), high = Math.max(...offsets);
  const padding = Math.max(1, (high - low) * 0.15);
  low -= padding; high += padding;
  const times = events.map((event) => safeOffset(event.timestamp_us, events[0].timestamp_us));
  const duration = Math.max(1, times[times.length - 1]);
  const x = (i) => left + (times[i] / duration) * (width - left - right);
  const y = (value) => top + (high - safeOffset(value, origin)) / (high - low) * (height - top - bottom);
  for (let tick = 0; tick <= 3; ++tick) {
    const offset = high - tick * (high - low) / 3;
    const py = top + tick * (height - top - bottom) / 3;
    svg.append(svgNode('line', { x1: left, x2: width - right, y1: py, y2: py, stroke: '#e6ece3', 'stroke-dasharray': '3 4' }));
    svg.append(svgNode('text', { x: left - 12, y: py + 4, 'text-anchor': 'end', fill: '#576863', 'font-size': 11 }, format(origin + BigInt(Math.round(offset)))));
  }
  for (const boundary of extraBounds) {
    svg.append(svgNode('line', { x1: left, x2: width - right, y1: y(boundary), y2: y(boundary), stroke: '#bcc8b8', 'stroke-dasharray': '6 5' }));
  }
  const ticks = [...new Set([0, Math.floor((events.length - 1) / 2), events.length - 1])];
  for (const i of ticks) {
    svg.append(svgNode('text', { x: x(i), y: height - 8, 'text-anchor': i === 0 ? 'start' : 'end', fill: '#576863', 'font-size': 11 }, `${units(events[i].timestamp_us)} µs`));
  }
  for (const line of series) {
    let path = '', active = false;
    for (let i = 0; i < events.length; ++i) {
      const value = line.value(events[i]);
      if (value === null) { active = false; continue; }
      path += `${active ? 'L' : 'M'}${x(i)},${y(value)} `;
      if (i + 1 < events.length) path += `L${x(i + 1)},${y(value)} `;
      active = true;
    }
    svg.append(svgNode('path', { d: path, fill: 'none', stroke: line.color, 'stroke-width': 2.3, 'stroke-linejoin': 'round' }));
    events.forEach((event, i) => {
      const value = line.value(event);
      if (value === null) return;
      const circle = svgNode('circle', { cx: x(i), cy: y(value), r: i === events.length - 1 ? 4 : 2.5, fill: line.color, stroke: '#fff', 'stroke-width': 1 });
      circle.append(svgNode('title', {}, `Event ${event.event_index} · ${event.timestamp_us} µs · ${line.label}: ${format(value)}`));
      svg.append(circle);
    });
  }
  container.replaceChildren(svg);
}

function render() {
  const event = scenario.events[position], history = scenario.events.slice(0, position + 1);
  $('status').textContent = `Showing state after event ${event.event_index}. Engine-generated snapshots; playback is for inspection.`;
  $('timeline').value = String(position);
  $('timeline').setAttribute('aria-valuetext', `Event ${position + 1} of ${scenario.events.length}, ${event.description}`);
  $('event-count').textContent = `${position + 1} / ${scenario.events.length}`;
  $('timestamp').textContent = `${units(event.timestamp_us)} µs`;
  $('event-type').textContent = event.description;
  $('reference').textContent = money(event.reference_price_paise);
  $('cash').textContent = money(event.cash_paise);
  $('inventory').textContent = `${units(event.inventory)} units`;
  $('inventory-limit').textContent = `Limit ±${units(scenario.strategy.inventory_limit)} units`;
  $('value').textContent = money(event.portfolio_value_paise);
  $('pnl').textContent = money(event.total_pnl_paise, true);
  $('pnl').className = BigInt(event.total_pnl_paise) < 0n ? 'negative' : BigInt(event.total_pnl_paise) > 0n ? 'positive' : '';
  for (const [id, side, label] of [['bid', 'maker_bid', 'BID'], ['ask', 'maker_ask', 'ASK']]) {
    const quote = event[side];
    $(id).textContent = quote ? `${label} ${money(quote.price_paise)} · ${units(quote.remaining_quantity)} unit · Order ${quote.order_id}` : `No active ${id}`;
  }
  drawChart($('price-chart'), history, [
    { label: 'Reference', color: colors.reference, value: (e) => e.reference_price_paise },
    { label: 'Maker bid', color: colors.bid, value: (e) => e.maker_bid?.price_paise ?? null },
    { label: 'Maker ask', color: colors.ask, value: (e) => e.maker_ask?.price_paise ?? null }
  ], money);
  const limit = BigInt(scenario.strategy.inventory_limit);
  // A distant configured limit may be outside exact plotting range for imports.
  // Display it in text; only draw boundary lines when the offsets are safe.
  let boundaries = [];
  try { safeOffset(limit, history[0].inventory); safeOffset(-limit, history[0].inventory); boundaries = [limit, -limit]; } catch { /* Text retains exact limits. */ }
  drawChart($('inventory-chart'), history, [{ label: 'Inventory', color: colors.inventory, value: (e) => e.inventory }], units, boundaries);
  let pnlBounds = [];
  try { safeOffset(0n, history[0].total_pnl_paise); pnlBounds = [0n]; } catch { /* Exact P&L still displayed. */ }
  drawChart($('pnl-chart'), history, [{ label: 'P&L', color: colors.pnl, value: (e) => e.total_pnl_paise }], (v) => money(v, true), pnlBounds);
  const rows = [];
  for (const past of history) for (const trade of past.trades) {
    const row = document.createElement('tr');
    for (const value of [`#${past.event_index} / ${units(past.timestamp_us)}`, trade.buy_id, trade.sell_id, money(trade.price_paise), units(trade.quantity)]) {
      const cell = document.createElement('td'); cell.textContent = value; row.append(cell);
    }
    rows.push(row);
  }
  $('trades').replaceChildren(...rows);
  $('trade-count').textContent = `${rows.length} ${rows.length === 1 ? 'FILL' : 'FILLS'}`;
  $('no-trades').hidden = rows.length > 0;
  $('event-log').replaceChildren(...history.map((past, i) => {
    const item = document.createElement('li'); item.className = i === position ? 'current' : '';
    const index = document.createElement('span'); index.className = 'event-index'; index.textContent = `#${past.event_index}`;
    const copy = document.createElement('div'); copy.textContent = past.description;
    const caption = document.createElement('small'); caption.textContent = `${units(past.timestamp_us)} µs · ${past.trades.length} ${past.trades.length === 1 ? 'fill' : 'fills'}`;
    copy.append(caption); item.append(index, copy); return item;
  }));
  controls();
}

$('previous').addEventListener('click', () => { pause(); setPosition(position - 1); });
$('next').addEventListener('click', () => { pause(); setPosition(position + 1); });
$('restart').addEventListener('click', () => { pause(); setPosition(0); });
$('play').addEventListener('click', play);
$('timeline').addEventListener('input', () => { pause(); setPosition(Number($('timeline').value)); });
$('speed').addEventListener('change', () => { if (timer) { pause(); play(); } });
$('scenario').addEventListener('change', () => {
  if ($('scenario').value === 'imported' && importedScenario) {
    ++request; pause(); applyData(importedScenario); loadedOption = 'imported';
  } else loadBundled($('scenario').value);
});
$('upload').addEventListener('change', async () => {
  const file = $('upload').files[0];
  if (!file) return;
  const token = ++request; pause(); controls(true);
  try {
    if (file.size > 10 * 1024 * 1024) throw new Error('File exceeds the 10 MB limit.');
    const data = JSON.parse(await file.text());
    if (token !== request) return;
    applyData(data);
    let option = [...$('scenario').options].find((entry) => entry.value === 'imported');
    if (!option) { option = new Option('', 'imported'); $('scenario').add(option); }
    importedScenario = scenario;
    option.textContent = `Imported · ${scenario.scenario}`;
    $('scenario').value = 'imported'; loadedOption = 'imported';
  } catch (error) { if (token === request) reportError(error, 'Invalid export'); }
  finally { $('upload').value = ''; }
});
loadBundled('inventory-risk');

window.addEventListener('resize', () => { if (scenario) render(); });
