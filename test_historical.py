#!/usr/bin/env python3
"""Synthetic parser edge fixtures plus optional source-to-export real-data audit."""
import argparse
import csv
from decimal import Decimal
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('prepare_historical', ROOT / 'scripts/prepare_historical.py')
prepare = importlib.util.module_from_spec(spec); spec.loader.exec_module(prepare)
BINARY = ROOT / 'historical_replay'


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def rows(path):
    with path.open(newline='') as stream:
        return list(csv.DictReader(stream))


def provenance(folder, snapshot, trades):
    metadata = ['Tardis', 'binance', 'BTCUSDT', 'spot', '2024-01-01', '1704067200000000',
                '1704067500000000', '2026-10-10T00:00:00+00:00',
                'https://datasets.tardis.dev/v1/binance/book_snapshot_5/2024/01/01/BTCUSDT.csv.gz',
                'https://datasets.tardis.dev/v1/binance/trades/2024/01/01/BTCUSDT.csv.gz',
                hashlib.sha256(snapshot.read_bytes()).hexdigest(), hashlib.sha256(trades.read_bytes()).hexdigest(),
                len(rows(snapshot)), len(rows(trades))]
    path = folder / 'provenance.csv'
    with path.open('w', newline='') as output:
        writer = csv.writer(output, lineterminator='\n'); writer.writerow(prepare.PROVENANCE_HEADER); writer.writerow(metadata)
    return path


def run(snapshot, trades, metadata, output):
    return subprocess.run([str(BINARY), '--snapshots', str(snapshot), '--trades', str(trades),
                           '--provenance', str(metadata), '--output', str(output)], text=True, capture_output=True)


def audit(export, snapshot, trades):
    data = json.loads(export.read_text())
    check(data['schema'] == 'historical-market' and data['schema_version'] == 1, 'historical discrimination/version')
    check(data['instrument']['market'] == 'spot' and data['units']['price'] == 'USDT per BTC', 'instrument units')
    streams = {'snapshot': rows(snapshot), 'trade': rows(trades)}
    expected = sorted((int(row['local_timestamp']), 0 if kind == 'snapshot' else 1, i + 1, kind)
                      for kind, source in streams.items() for i, row in enumerate(source))
    actual = [(int(event['timestamp_us']), 0 if event['type'] == 'snapshot' else 1, int(event['source_row']), event['type'])
              for event in data['events']]
    check(actual == expected, 'stable capture-time merge / per-file row order')
    observed, seen = None, {'snapshot': set(), 'trade': set()}
    for index, event in enumerate(data['events']):
        check(event['event_index'] == str(index), 'increasing event index')
        kind = event['type']; source_row = int(event['source_row'])
        check(source_row not in seen[kind], 'source event repeated'); seen[kind].add(source_row)
        row = streams[kind][source_row - 1]
        check(event['exchange_timestamp_us'] == row['timestamp'] and event['timestamp_us'] == row['local_timestamp'], 'timestamps changed')
        if kind == 'snapshot':
            observed = event['snapshot']
            for side in ('bids', 'asks'):
                expected_levels = [{'price': row[f'{side}[{i}].price'], 'amount': row[f'{side}[{i}].amount']}
                                   for i in range(5) if row[f'{side}[{i}].price']]
                check(observed[side] == expected_levels, 'source depth decimals changed')
                best = 'best_bid' if side == 'bids' else 'best_ask'
                check(observed[best] == (expected_levels[0]['price'] if expected_levels else None), 'best level mismatch')
            if observed['bids'] and observed['asks']:
                bid, ask = Decimal(observed['best_bid']), Decimal(observed['best_ask'])
                check(Decimal(observed['spread']) == ask - bid, 'spread exactness')
                check(Decimal(observed['midpoint']) == (ask + bid) / 2, 'midpoint exactness')
            else:
                check(observed['spread'] is None and observed['midpoint'] is None, 'missing metrics')
        else:
            check('snapshot' not in event, 'trade mutated or duplicated snapshot state')
            check(event['trade'] == {key: row[key] for key in ('id', 'side', 'price', 'amount')}, 'recorded trade changed')
        # No snapshots are attached to trades. Before first snapshot, state stays None.
        if index == 0 and kind == 'trade': check(observed is None, 'look-ahead book populated initial trade')
    check(len(seen['snapshot']) == len(streams['snapshot']) and len(seen['trade']) == len(streams['trade']), 'source rows excluded')
    return data


def downloader_checks(folder):
    header = ','.join(prepare.TRADE_HEADER) + '\n'
    body = header + 'binance,BTCUSDT,1,100,id,buy,100,0.1\n' + 'binance,BTCUSDT,2,200,id2,sell,100,0.2\n'
    class Headers:
        def __init__(self, content='text/csv'): self.content = content
        def get_content_type(self): return self.content
    class Response(io.BytesIO):
        status = 200
        def __init__(self, data, content='text/csv'):
            super().__init__(data); self.headers = Headers(content)
    target = folder / 'extract.csv'
    with patch.object(prepare.urllib.request, 'urlopen', return_value=Response(gzip.compress(body.encode()))):
        result = prepare.extract_window('https://test.invalid', target, prepare.TRADE_HEADER, 100, 200, 10000, 1, 10)
        check(result['rows'] == 1 and len(rows(target)) == 1 and rows(target)[0]['amount'] == '0.1', 'bounded extraction')
    for data, content, limit in [(gzip.compress(body.encode()), 'text/csv', 1),
                                 (b'<html>error</html>', 'text/html', 10000),
                                 (b'not gzip', 'text/csv', 10000),
                                 (gzip.compress(b'error,message\nnot,CSV\n'), 'text/csv', 10000)]:
        with patch.object(prepare.urllib.request, 'urlopen', return_value=Response(data, content)):
            try: prepare.extract_window('https://test.invalid', folder / 'bad.csv', prepare.TRADE_HEADER, 100, 200, limit, 1, 10)
            except Exception: pass
            else: raise AssertionError('bad download accepted')
        check(not (folder / 'bad.csv.part').exists(), 'partial extract not cleaned')
    # Missing boundary / unexpected capture regression must fail, not sort or truncate silently.
    for bad in [header + 'binance,BTCUSDT,1,100,id,buy,100,0.1\n',
                body.replace(',2,200,', ',2,99,')]:
        with patch.object(prepare.urllib.request, 'urlopen', return_value=Response(gzip.compress(bad.encode()))):
            try: prepare.extract_window('https://test.invalid', folder / 'bad.csv', prepare.TRADE_HEADER, 100, 200, 10000, 1, 10)
            except Exception: pass
            else: raise AssertionError('incomplete/unordered stream accepted')


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--actual', type=Path); args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp); fixture = ROOT / 'test_fixtures/historical'
        snapshot, trades = folder / 'snapshots.csv', folder / 'trades.csv'
        shutil.copyfile(fixture / 'book_snapshot_5.csv', snapshot); shutil.copyfile(fixture / 'trades.csv', trades)
        metadata = provenance(folder, snapshot, trades); export = folder / 'output.json'
        result = run(snapshot, trades, metadata, export)
        check(result.returncode == 0, result.stderr)
        data = audit(export, snapshot, trades)
        check([event['type'] for event in data['events']] == ['trade', 'snapshot', 'snapshot', 'trade', 'trade'], 'cross-file tie')
        check(data['events'][1]['snapshot']['midpoint'] == '100.000000005', 'ninth-place midpoint rounded')
        check(data['events'][2]['snapshot']['bids'][0]['amount'] == '8.00000001', 'snapshot not replaced')
        check(data['events'][3]['trade']['id'] == 'fixture,"quoted"', 'CSV/JSON escaping')
        check(data['anomalies'], 'missing levels/regression anomalies not reported')
        first_bytes = export.read_bytes(); check(run(snapshot, trades, metadata, export).returncode == 0, 'repeat export failed')
        check(export.read_bytes() == first_bytes, 'exports not deterministic')
        original_snapshot, original_trades = snapshot.read_text(), trades.read_text()
        invalid_snapshots = [original_snapshot.replace('asks[0].price', 'unknown', 1),
                             original_snapshot.replace('100.00000001', '1e2', 1),
                             original_snapshot.replace('0.00000001', '0', 1),
                             original_snapshot.replace('100.00000001', '100.000000001', 1),
                             original_snapshot.replace('100.00000001', '999999999999999999999999', 1),
                             original_snapshot.replace(',100,1.23456789,', ',101,1.23456789,'),
                             original_snapshot.replace(',101,2,', ',99,2,'),
                             original_snapshot.replace(',100,1.23456789,', ',100,,'),
                             original_snapshot.replace('binance,BTCUSDT', 'binance-futures,BTCUSDT'),
                             original_snapshot.replace('1704067200000300', '1704067200000100')]
        for bad in invalid_snapshots:
            snapshot.write_text(bad); export.write_text('keep me')
            result = run(snapshot, trades, metadata, export)
            check(result.returncode != 0 and 'Exported' not in result.stdout and export.read_text() == 'keep me', 'invalid snapshot accepted or truncated output')
        snapshot.write_text(original_snapshot)
        for bad in [original_trades.replace(',buy,', ',invalid,'), original_trades.replace(',100.12345678,', ',1.5junk,'),
                    original_trades.replace('1704067200000300', '1704067200000001', 1),
                    original_trades.replace('1704067200000100', '18446744073709551616', 1)]:
            trades.write_text(bad)
            check(run(snapshot, trades, metadata, export).returncode != 0, 'invalid trade accepted')
        trades.write_text(original_trades)
        # Empty depth is distinct from zero; trailing missing levels remain valid.
        with snapshot.open('w', newline='') as stream:
            writer = csv.writer(stream, lineterminator='\n'); writer.writerow(prepare.SNAPSHOT_HEADER)
            writer.writerow(['binance', 'BTCUSDT', '1704067200000195', '1704067200000200'] + [''] * 20)
        metadata = provenance(folder, snapshot, trades)
        check(run(snapshot, trades, metadata, export).returncode == 0, 'valid empty observed book rejected')
        empty = json.loads(export.read_text())['events'][1]['snapshot']
        check(empty['bids'] == [] and empty['asks'] == [] and empty['midpoint'] is None, 'empty depth became zero')
        # Known file failures and aliases are fatal, without a success message.
        for path in [snapshot, folder, folder / 'missing' / 'output.json'] + ([Path('/dev/full')] if Path('/dev/full').exists() else []):
            result = run(snapshot, trades, metadata, path)
            check(result.returncode != 0 and 'Exported' not in result.stdout, 'output failure accepted')
        check(subprocess.run([str(BINARY), '--unknown'], capture_output=True).returncode != 0, 'bad invocation accepted')
        downloader_checks(folder)
        if args.actual:
            actual = args.actual.resolve(); s = actual / 'book_snapshot_5.csv'; t = actual / 'trades.csv'; p = actual / 'provenance.csv'
            manifest = json.loads((actual / 'manifest.json').read_text())
            for kind, path in [('book_snapshot_5', s), ('trades', t)]:
                check(hashlib.sha256(path.read_bytes()).hexdigest() == manifest['files'][kind]['sha256'], 'retained checksum mismatch')
            output, again = folder / 'actual.json', folder / 'actual-again.json'
            result = run(s, t, p, output); check(result.returncode == 0, result.stderr)
            actual_data = audit(output, s, t)
            check(run(s, t, p, again).returncode == 0 and output.read_bytes() == again.read_bytes(), 'actual export not repeatable')
            check(output.read_bytes() == (ROOT / 'dashboard/data/historical/btcusdt.json').read_bytes(), 'local dashboard export stale')
            from datetime import datetime, timezone
            for kind in ('snapshot', 'trade'):
                selected = [e for e in actual_data['events'] if e['type'] == kind]
                print(f'ACTUAL {kind}: {len(selected)} rows; capture {selected[0]["timestamp_us"]}..{selected[-1]["timestamp_us"]}; exchange {selected[0]["exchange_timestamp_us"]}..{selected[-1]["exchange_timestamp_us"]}')
            print('ACTUAL source comparisons: every exported snapshot level and recorded trade checked against its retained CSV row')
            print('ACTUAL anomalies:', actual_data['anomalies'])
    print('PASS: historical parser, exact decimals, stable merge, snapshot lifecycle, provenance audit, download bounds, and file errors')


if __name__ == '__main__':
    main()
