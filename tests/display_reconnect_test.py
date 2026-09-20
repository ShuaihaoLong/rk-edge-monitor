#!/usr/bin/env python3
"""Board-only: interrupt a private RTSP relay; leave production camera/recording untouched."""
import argparse
import os
import signal
import subprocess
import time
from pathlib import Path


def stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise RuntimeError('process did not stop within 10s')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--display', required=True)
    parser.add_argument('--directory', required=True)
    args = parser.parse_args()
    directory = Path(args.directory)
    directory.mkdir(parents=True, exist_ok=True)
    relay_config = directory / 'relay.yml'
    relay_config.write_text('''logLevel: info
rtspAddress: 127.0.0.1:18554
rtspTransports: [tcp]
rtmp: false
hls: false
webrtc: false
srt: false
moq: false
paths:
  camera:
    source: rtsp://127.0.0.1:8554/camera
''')
    config = directory / 'rkmon.ini'
    original = Path('/opt/rkmon/config/rkmon.ini').read_text()
    if 'rtsp://127.0.0.1:8554/camera' not in original:
        raise RuntimeError('unexpected stream URL')
    config.write_text(original.replace('rtsp://127.0.0.1:8554/camera', 'rtsp://127.0.0.1:18554/camera'))
    # Convert existing relative application paths to absolute for standalone configuration.
    config.write_text(config.read_text().replace('../models/', '/opt/rkmon/models/').replace('../logs/', '/opt/rkmon/logs/'))
    relay = display = None
    env = dict(os.environ, XDG_RUNTIME_DIR='/run/user/1000', WAYLAND_DISPLAY='wayland-0', SDL_VIDEODRIVER='wayland')
    with (directory / 'relay.log').open('w') as relay_log, (directory / 'display.log').open('w') as display_log:
        def start_relay():
            return subprocess.Popen(['/opt/rkmon/bin/mediamtx', str(relay_config)], stdout=relay_log, stderr=subprocess.STDOUT)
        try:
            relay = start_relay()
            time.sleep(3)
            if relay.poll() is not None:
                raise RuntimeError('relay startup failed')
            display = subprocess.Popen([args.display, '--config', str(config), '--settings', '/tmp/rkmon-lvgl-test/display.json'],
                                       env=env, stdout=display_log, stderr=subprocess.STDOUT)
            time.sleep(10)
            stop(relay)
            time.sleep(9)
            relay = start_relay()
            time.sleep(12)
            if display.poll() is not None:
                raise RuntimeError('display exited during outage')
            began = time.monotonic()
            stop(display)
            elapsed = time.monotonic() - began
            if display.returncode != 0:
                raise RuntimeError(f'display exit={display.returncode}')
            content = (directory / 'display.log').read_text()
            if content.count('[display/video] connected') < 2:
                raise RuntimeError('video did not reconnect')
            print(f'PASS relay outage/recovery; display SIGTERM shutdown={elapsed:.3f}s')
        finally:
            stop(display)
            stop(relay)


if __name__ == '__main__':
    main()
