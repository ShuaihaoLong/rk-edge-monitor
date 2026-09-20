#!/usr/bin/env python3
"""Update the local display weather cache with IP location and Hefei fallback."""
import argparse
import json
import math
import os
import signal
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path

FALLBACK = {
    'city': '合肥',
    'latitude': 31.8206,
    'longitude': 117.2272,
    'location_source': 'fallback',
}


def fetch(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'rkmon-display/1.0'})
    with urllib.request.urlopen(request, timeout=5) as response:
        data = response.read(65537)
        if len(data) > 65536:
            raise ValueError('response exceeds 64 KiB')
        result = json.loads(data)
        if not isinstance(result, dict):
            raise ValueError('expected JSON object')
        return result


def numeric(value, lower, upper):
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and lower <= value <= upper
    )


def location(fetcher=fetch):
    try:
        data = fetcher('https://ipapi.co/json/')
        if (
            data.get('error')
            or not numeric(data.get('latitude'), -90, 90)
            or not numeric(data.get('longitude'), -180, 180)
        ):
            raise ValueError('invalid location')
        city = data.get('city')
        if not isinstance(city, str) or not city.strip():
            raise ValueError('missing city')
        return {
            'city': city[:40],
            'latitude': data['latitude'],
            'longitude': data['longitude'],
            'location_source': 'ip',
        }
    except (OSError, ValueError, TypeError):
        return dict(FALLBACK)


def description(code):
    if code == 0:
        return '晴'
    if code in (1, 2, 3):
        return '多云' if code != 3 else '阴'
    if code in (45, 48):
        return '雾'
    if code in (51, 53, 55, 56, 57):
        return '毛毛雨'
    if code in (61, 63, 65, 66, 67, 80, 81, 82):
        return '雨'
    if code in (71, 73, 75, 77, 85, 86):
        return '雪'
    if code in (95, 96, 99):
        return '雷雨'
    return '天气代码 ' + str(code)


def update(previous, fetcher=fetch, now=None):
    now = time.time() if now is None else now
    place = location(fetcher)
    query = urllib.parse.urlencode(
        {
            'latitude': place['latitude'],
            'longitude': place['longitude'],
            'current': 'temperature_2m,weather_code',
            'timezone': 'auto',
        }
    )
    try:
        data = fetcher('https://api.open-meteo.com/v1/forecast?' + query)
        current = data['current']
        temperature, code = current['temperature_2m'], current['weather_code']
        if not numeric(temperature, -100, 70) or not numeric(code, 0, 99) or int(code) != code:
            raise ValueError('invalid weather values')
        return dict(
            place,
            schema=1,
            status='ok',
            temperature_c=temperature,
            description=description(code),
            weather_code=code,
            updated_at=now,
            attempted_at=now,
            source_time=current.get('time', ''),
            source='Open-Meteo',
        )
    except (OSError, ValueError, TypeError, KeyError):
        # 保留城市和读数配对，定位变化但天气失败时不能把旧读数标成新城市。
        if isinstance(previous, dict) and numeric(previous.get('updated_at'), 1, now + 60):
            return dict(previous, status='offline', attempted_at=now)
        return dict(place, schema=1, status='offline', updated_at=0, attempted_at=now)


def write_cache(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.weather-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as output:
            json.dump(data, output, ensure_ascii=False, allow_nan=False)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(name, 0o644)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--settings', default='/opt/rkmon/config/display.json')
    parser.add_argument('--once', action='store_true')
    args = parser.parse_args()
    settings = json.loads(Path(args.settings).read_text())
    path = Path(settings['weather_cache'])
    interval = settings.get('weather_interval_seconds', 900)
    if not isinstance(interval, int) or isinstance(interval, bool) or not 60 <= interval <= 86400:
        parser.error('weather_interval_seconds must be 60..86400')
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    previous = {}
    try:
        if path.stat().st_size <= 8192:
            previous = json.loads(path.read_text())
    except (OSError, ValueError):
        pass
    while not stop.is_set():
        previous = update(previous)
        write_cache(path, previous)
        print(f"weather status={previous['status']} city={previous['city']}", flush=True)
        if args.once:
            break
        stop.wait(interval if previous['status'] == 'ok' else min(interval, 60))


if __name__ == '__main__':
    main()
