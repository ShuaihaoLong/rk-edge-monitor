#!/usr/bin/env python3
"""Read-only /proc sampling on the board; run before and during the display probe."""
import argparse
import json
import os
import time
from pathlib import Path


def snapshot():
    result = {}
    for path in Path('/proc').glob('[0-9]*/stat'):
        try:
            text = path.read_text()
            name = text[text.index('(') + 1:text.rindex(')')]
            if name not in ('rkmon', 'lvgl-video-prob', 'rkmon-display', 'gnome-shell', 'mediamtx'):
                continue
            parts = text[text.rindex(')') + 2:].split()
            result[path.parent.name] = dict(name=name, ticks=int(parts[11]) + int(parts[12]),
                                            rss_kib=int(parts[21]) * os.sysconf('SC_PAGE_SIZE') // 1024)
        except (OSError, ValueError, IndexError):
            continue
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=60)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600:
        parser.error('seconds must be 1..3600')
    start = time.monotonic()
    before = snapshot()
    peak = {pid: row['rss_kib'] for pid, row in before.items()}
    while time.monotonic() - start < args.seconds:
        time.sleep(1)
        after = snapshot()
        for pid, row in after.items():
            peak[pid] = max(peak.get(pid, 0), row['rss_kib'])
    elapsed = time.monotonic() - start
    for pid, row in after.items():
        if pid in before:
            row['cpu_percent'] = (row['ticks'] - before[pid]['ticks']) / os.sysconf('SC_CLK_TCK') / elapsed * 100
        row['peak_rss_kib'] = peak[pid]
    print(json.dumps(dict(seconds=elapsed, processes=after), indent=2))


if __name__ == '__main__':
    main()
