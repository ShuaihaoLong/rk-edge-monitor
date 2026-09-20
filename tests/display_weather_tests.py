#!/usr/bin/env python3
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('weather', Path(__file__).resolve().parents[1] / 'src/display/weather.py')
weather = importlib.util.module_from_spec(spec)
spec.loader.exec_module(weather)


class WeatherTests(unittest.TestCase):
    def test_location_failure_uses_hefei(self):
        calls = []
        def request(url):
            calls.append(url)
            if 'ipapi' in url:
                raise OSError('offline location')
            return {'current': {'temperature_2m': 24, 'weather_code': 3}}
        result = weather.update({}, request, 1000)
        self.assertEqual(result['city'], '合肥')
        self.assertEqual(result['status'], 'ok')
        self.assertIn('latitude=31.8206', calls[1])

    def test_ip_location_and_weather(self):
        def request(url):
            if 'ipapi' in url:
                return {'city': 'Shanghai', 'latitude': 31.2, 'longitude': 121.5}
            self.assertIn('longitude=121.5', url)
            return {'current': {'temperature_2m': 26.2, 'weather_code': 0}}
        self.assertEqual(weather.update({}, request, 1000)['city'], 'Shanghai')

    def test_failure_preserves_city_and_cache_age(self):
        previous = {'city': '合肥', 'temperature_c': 23, 'updated_at': 900}
        def request(url):
            if 'ipapi' in url:
                return {'city': 'Shanghai', 'latitude': 31.2, 'longitude': 121.5}
            raise OSError('no weather')
        result = weather.update(previous, request, 1000)
        self.assertEqual(result['city'], '合肥')
        self.assertEqual(result['updated_at'], 900)
        self.assertEqual(result['status'], 'offline')

    def test_invalid_numbers_and_no_fake_temperature(self):
        def request(url):
            if 'ipapi' in url:
                return {'city': 'bad', 'latitude': True, 'longitude': 4}
            return {'current': {'temperature_2m': float('nan'), 'weather_code': 0}}
        result = weather.update({}, request, 1000)
        self.assertEqual(result['city'], '合肥')
        self.assertNotIn('temperature_c', result)
        self.assertEqual(result['updated_at'], 0)

    def test_atomic_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'weather.json'
            weather.write_cache(path, {'city': '合肥', 'updated_at': 0})
            self.assertEqual(json.loads(path.read_text())['city'], '合肥')
            self.assertEqual(list(Path(directory).iterdir()), [path])


if __name__ == '__main__':
    unittest.main()
