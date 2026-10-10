#!/usr/bin/env python3
"""Retain a bounded capture-time window from Tardis gzip CSV streams.
Retained .csv files are extracts, NOT complete daily archives. No API key needed
for first-of-month samples. Raw data/derived tick exports must remain local.
"""
import argparse
import csv
from datetime import datetime, timedelta, timezone
import gzip
import hashlib
import io
import json
from pathlib import Path
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SNAPSHOT_HEADER = ['exchange', 'symbol', 'timestamp', 'local_timestamp'] + [
    field for level in range(5) for field in
    (f'asks[{level}].price', f'asks[{level}].amount', f'bids[{level}].price', f'bids[{level}].amount')]
TRADE_HEADER = ['exchange', 'symbol', 'timestamp', 'local_timestamp', 'id', 'side', 'price', 'amount']
PROVENANCE_HEADER = ['provider', 'exchange', 'symbol', 'market', 'date', 'window_start_us', 'window_end_us',
                     'retrieved_at_utc', 'snapshot_url', 'trade_url', 'snapshot_sha256', 'trade_sha256',
                     'snapshot_rows', 'trade_rows']


class LimitedResponse:
    def __init__(self, response, maximum, deadline):
        self.response, self.maximum, self.deadline = response, maximum, deadline
        self.count = 0

    def read(self, count=-1):
        if time.monotonic() > self.deadline:
            raise RuntimeError('Total download deadline exceeded')
        if self.count >= self.maximum:
            raise RuntimeError(f'Compressed download limit exceeded ({self.maximum} bytes)')
        # Gzip asks for bounded chunks; never allow an unrestricted daily read.
        count = min(count if count >= 0 else 65536, self.maximum - self.count)
        chunk = self.response.read(count)
        self.count += len(chunk)
        if time.monotonic() > self.deadline:
            raise RuntimeError('Total download deadline exceeded')
        return chunk


def extract_window(url, target, header, start, end, max_bytes, timeout, max_seconds):
    request = urllib.request.Request(url, headers={'User-Agent': 'trading-engine-historical-viewer/1.0'})
    retained, scanned, previous = 0, 0, -1
    exchange_regressions, previous_exchange = 0, -1
    decoded_bytes = 0
    temporary = target.with_suffix('.csv.part')
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            if response.status != 200:
                raise RuntimeError(f'Unexpected HTTP status: {response.status}')
            content_type = response.headers.get_content_type()
            if content_type not in ('text/csv', 'application/gzip', 'application/x-gzip', 'application/octet-stream'):
                raise RuntimeError(f'Non-CSV response Content-Type: {content_type}')
            limited = LimitedResponse(response, max_bytes, time.monotonic() + max_seconds)
            with gzip.GzipFile(fileobj=limited) as decompressed:
                def lines():
                    nonlocal decoded_bytes
                    while True:
                        raw = decompressed.readline(16385)
                        if not raw:
                            return
                        if len(raw) > 16384:
                            raise RuntimeError('CSV line exceeds 16 KiB limit')
                        decoded_bytes += len(raw)
                        if decoded_bytes > 32 * 1024 * 1024:
                            raise RuntimeError('Decompressed extract exceeds 32 MiB scan limit')
                        yield raw.decode('utf-8', errors='strict')
                reader = csv.reader(lines(), strict=True)
                if next(reader, None) != header:
                    raise RuntimeError('Response has an unexpected CSV header (error body or unsupported schema)')
                with temporary.open('w', encoding='utf-8', newline='') as output:
                    writer = csv.writer(output, lineterminator='\n')
                    writer.writerow(header)
                    reached_boundary = False
                    for row in reader:
                        scanned += 1
                        if scanned > 200000:
                            raise RuntimeError('CSV scan exceeds 200000-row limit')
                        if len(row) != len(header) or row[:2] != ['binance', 'BTCUSDT']:
                            raise RuntimeError(f'Unsupported instrument/row shape at input row {scanned}')
                        if any('\n' in field or '\r' in field for field in row):
                            raise RuntimeError('Multiline CSV fields are unsupported')
                        if not row[2].isascii() or not row[2].isdigit() or not row[3].isascii() or not row[3].isdigit():
                            raise RuntimeError(f'Invalid timestamp at input row {scanned}')
                        exchange_time, capture = int(row[2]), int(row[3])
                        if not 0 <= exchange_time < 2**64 or not 0 <= capture < 2**64:
                            raise RuntimeError(f'Timestamp overflow at input row {scanned}')
                        if capture < previous:
                            raise RuntimeError(f'Capture timestamp moved backward at input row {scanned}; no sorting applied')
                        previous = capture
                        if capture >= end:
                            reached_boundary = True
                            break
                        if capture < start:
                            continue
                        if exchange_time < previous_exchange:
                            exchange_regressions += 1
                        previous_exchange = exchange_time
                        writer.writerow(row)
                        retained += 1
                if not reached_boundary:
                    raise RuntimeError('Daily stream ended before the requested window boundary')
                if not retained:
                    raise RuntimeError('Requested window contains no retained rows')
            compressed_read = limited.count
        temporary.replace(target)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    return {'file': target.name, 'source_url': url, 'rows': retained, 'scanned_rows': scanned,
            'sha256': hashlib.sha256(target.read_bytes()).hexdigest(), 'bytes': target.stat().st_size,
            'compressed_bytes_read': compressed_read, 'decompressed_bytes_scanned': decoded_bytes,
            'exchange_timestamp_regressions': exchange_regressions}


def verify_cached_window(destination, date, start, end):
    manifest = json.loads((destination / 'manifest.json').read_text(encoding='utf-8'))
    expected = {'provider': 'Tardis', 'exchange': 'binance', 'symbol': 'BTCUSDT', 'market': 'spot',
                'date': date, 'window_start_us': str(start), 'window_end_us': str(end)}
    if any(manifest.get(key) != value for key, value in expected.items()):
        raise RuntimeError('Cached manifest does not describe the requested instrument/window')
    for kind, header in [('book_snapshot_5', SNAPSHOT_HEADER), ('trades', TRADE_HEADER)]:
        source = destination / f'{kind}.csv'
        details = manifest['files'][kind]
        if source.stat().st_size > 32 * 1024 * 1024 or hashlib.sha256(source.read_bytes()).hexdigest() != details['sha256']:
            raise RuntimeError(f'Cached retained-file checksum/size mismatch: {source}')
        with source.open(newline='', encoding='utf-8') as stream:
            reader = csv.reader(stream, strict=True)
            if next(reader, None) != header or sum(1 for _ in reader) != details['rows']:
                raise RuntimeError(f'Cached header/row-count mismatch: {source}')
        expected_url = f'https://datasets.tardis.dev/v1/binance/{kind}/{date.replace("-", "/")}/BTCUSDT.csv.gz'
        if details['source_url'] != expected_url:
            raise RuntimeError('Cached source URL mismatch')
    f = manifest['files']
    expected_row = ['Tardis', 'binance', 'BTCUSDT', 'spot', date, str(start), str(end), manifest['retrieved_at_utc'],
                    f['book_snapshot_5']['source_url'], f['trades']['source_url'],
                    f['book_snapshot_5']['sha256'], f['trades']['sha256'], str(f['book_snapshot_5']['rows']), str(f['trades']['rows'])]
    with (destination / 'provenance.csv').open(newline='', encoding='utf-8') as stream:
        if list(csv.reader(stream, strict=True)) != [PROVENANCE_HEADER, expected_row]:
            raise RuntimeError('Cached provenance sidecar disagrees with manifest')
    print(f'Reusing checksum-verified retained window: {destination}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--date', default='2024-01-01', help='A documented first-of-month UTC date')
    parser.add_argument('--directory', type=Path, default=ROOT / '.local_data/binance-BTCUSDT')
    parser.add_argument('--max-download-mib', type=int, default=10, help='Per-response compressed byte limit')
    parser.add_argument('--timeout', type=int, default=20, help='Socket timeout seconds')
    parser.add_argument('--max-seconds', type=int, default=120, help='Total deadline per response')
    args = parser.parse_args()
    date = datetime.strptime(args.date, '%Y-%m-%d').replace(tzinfo=timezone.utc)
    if date.strftime('%Y-%m-%d') != args.date or date.day != 1:
        parser.error('Use a YYYY-MM-01 first-of-month date')
    if not 1 <= args.max_download_mib <= 100 or not 1 <= args.timeout <= 60 or not 1 <= args.max_seconds <= 600:
        parser.error('Limits must be positive and bounded: download ≤100 MiB, socket timeout ≤60 s, deadline ≤600 s')
    start = int(date.timestamp()) * 1000000
    end = start + 300 * 1000000
    args.directory.mkdir(parents=True, exist_ok=True)
    # A fresh subdirectory prevents stale manifests from blessing a failed run.
    destination = args.directory / args.date
    if destination.exists():
        verify_cached_window(destination, args.date, start, end)
        return
    destination.mkdir()
    files = {}
    for kind, header in [('book_snapshot_5', SNAPSHOT_HEADER), ('trades', TRADE_HEADER)]:
        url = f'https://datasets.tardis.dev/v1/binance/{kind}/{date:%Y/%m/%d}/BTCUSDT.csv.gz'
        files[kind] = extract_window(url, destination / f'{kind}.csv', header, start, end,
                                    args.max_download_mib * 1024 * 1024, args.timeout, args.max_seconds)
        print(f'{kind}: retained {files[kind]["rows"]} rows; read {files[kind]["compressed_bytes_read"]} compressed bytes')
    retrieved = datetime.now(timezone.utc).isoformat()
    manifest = {
        'manifest_version': 1, 'provider': 'Tardis', 'exchange': 'binance', 'symbol': 'BTCUSDT', 'market': 'spot',
        'date': args.date, 'window_start_utc': date.isoformat(), 'window_end_utc': (date + timedelta(minutes=5)).isoformat(),
        'window_start_us': str(start), 'window_end_us': str(end), 'retrieved_at_utc': retrieved,
        'files': files, 'units': {'price': 'USDT per BTC', 'quantity': 'BTC', 'timestamps': 'UTC epoch microseconds'},
        'ordering': 'local_timestamp ascending, original row order within each stream; snapshot before trade for cross-file ties (convention, not exchange-wide causality)',
        'filtering': 'Capture/local timestamp in [00:00:00,00:05:00) UTC. Exchange timestamps retained even if outside that interval. No rows sorted, deduplicated, or silently dropped inside window.',
        'normalization': 'UTF-8 CSV parsed and re-serialized with LF newlines; decimal strings unchanged. No complete gzip archives saved.',
        'retained_files': 'Window extracts, not complete daily archives; gzip streams intentionally closed after first boundary row.',
        'redistribution': 'Local-only: raw tick data and derived tick exports not tracked or publicly redistributed. See https://docs.tardis.dev/legal/terms-of-service',
        'limits': {'compressed_bytes_per_stream': args.max_download_mib * 1024 * 1024, 'socket_timeout_seconds': args.timeout,
                   'deadline_seconds_per_stream': args.max_seconds, 'decompressed_scan_bytes': 32 * 1024 * 1024, 'scan_rows': 200000},
    }
    # Flat provenance sidecar avoids a C++ JSON parser dependency. It mirrors the
    # human-readable JSON manifest; exporter validates this exact header/record.
    with (destination / 'provenance.csv').open('w', newline='', encoding='utf-8') as output:
        writer = csv.writer(output, lineterminator='\n')
        writer.writerow(PROVENANCE_HEADER)
        writer.writerow(['Tardis', 'binance', 'BTCUSDT', 'spot', args.date, start, end, retrieved,
                         files['book_snapshot_5']['source_url'], files['trades']['source_url'],
                         files['book_snapshot_5']['sha256'], files['trades']['sha256'],
                         files['book_snapshot_5']['rows'], files['trades']['rows']])
    (destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'Prepared local window: {destination}')


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        raise SystemExit(f'Historical preparation failed: {error}')
