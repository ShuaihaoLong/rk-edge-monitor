"""设备录像索引、回放路由、事件聚合及容量管理。"""
import argparse
import cgi
import configparser
from contextlib import contextmanager
import datetime as dt
import json
import logging
import math
import os
from pathlib import Path
import shutil
import signal
import socket
import sqlite3
import subprocess
import threading
import tempfile
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlencode, urlsplit
from zoneinfo import ZoneInfo

LOG = logging.getLogger('recording')
UTC = dt.timezone.utc


def configuration(path):
    parser = configparser.ConfigParser()
    if not parser.read(path):
        raise ValueError('录像配置文件不存在')
    c = parser['recording']
    result = dict(root=Path(c['root']).resolve(), socket=c['event_socket'],
                  host=c.get('host', '127.0.0.1'), port=c.getint('port', 9010),
                  timezone=c.get('timezone', 'Asia/Shanghai'), days=c.getint('retain_days', 7),
                  reserve=c.getint('reserve_mib', 1024) * 1024**2,
                  max_bytes=c.getint('max_gib', 0) * 1024**3,
                  ads_root=Path(c.get('ads_root', '/userdata/rkmon-ads')).resolve())
    if result['host'] != '127.0.0.1' or result['days'] < 1 or result['reserve'] < 64 * 1024**2 or result['max_bytes'] < 0:
        raise ValueError('invalid recording configuration')
    ZoneInfo(result['timezone'])
    return result


class Store:
    def __init__(self, config):
        self.config = config
        self.root = config['root']
        self.root.mkdir(parents=True, exist_ok=True)
        self.db_path = self.root / 'index.sqlite3'
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.error = ''
        self.last_scan = 0
        self.last_event = 0
        self.active = {}
        self.seen_revision = {}
        self.last_flush = 0
        with self.connect() as db:
            db.executescript('''
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS recordings(
                    id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, channel TEXT NOT NULL,
                    start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL DEFAULT 0,
                    duration REAL NOT NULL DEFAULT 0, bytes INTEGER NOT NULL DEFAULT 0,
                    mtime_ns INTEGER NOT NULL DEFAULT 0, status TEXT NOT NULL,
                    updated_ms INTEGER NOT NULL);
                CREATE INDEX IF NOT EXISTS recording_time ON recordings(channel,start_ms);
                CREATE TABLE IF NOT EXISTS events(
                    id INTEGER PRIMARY KEY, session TEXT NOT NULL, generation INTEGER NOT NULL,
                    class_id INTEGER NOT NULL, label TEXT NOT NULL, start_ms INTEGER NOT NULL,
                    end_ms INTEGER NOT NULL, confidence REAL NOT NULL, sequence INTEGER NOT NULL);
                CREATE INDEX IF NOT EXISTS event_time ON events(start_ms,end_ms);
            ''')

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.db_path, timeout=5)
        db.row_factory = sqlite3.Row
        db.execute('PRAGMA busy_timeout=5000')
        try:
            with db:
                yield db
        finally:
            db.close()

    def safe_file(self, relative):
        path = (self.root / relative).resolve()
        if not path.is_relative_to(self.root / 'camera') or path.suffix != '.mp4':
            raise ValueError('invalid recording path')
        return path

    def file_start(self, path):
        # 文件名为 UTC；路径时间不采用本机当前时区解析。
        relative = path.relative_to(self.root)
        if len(relative.parts) != 3 or relative.parts[0] != 'camera':
            raise ValueError('invalid recording filename')
        stamp = dt.datetime.strptime(relative.parts[1] + '_' + path.stem, '%Y-%m-%d_%H-%M-%S-%f')
        return int(stamp.replace(tzinfo=UTC).timestamp() * 1000)

    def inspect(self, path, completed=False):
        path = self.safe_file(str(path))
        before = path.stat()
        start = self.file_start(path)
        result = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                                 '-show_entries', 'stream=codec_name:format=duration', '-of', 'json', str(path)],
                                capture_output=True, text=True, timeout=8, check=True)
        data = json.loads(result.stdout)
        duration = float(data['format']['duration'])
        if not math.isfinite(duration) or duration <= 0 or duration > 86400 or not data.get('streams'):
            raise ValueError('invalid recording duration')
        after = path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            return
        relative = str(path.relative_to(self.root))
        with self.lock, self.connect() as db:
            db.execute('''INSERT INTO recordings(path,channel,start_ms,end_ms,duration,bytes,mtime_ns,status,updated_ms)
                VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET end_ms=excluded.end_ms,
                duration=excluded.duration,bytes=excluded.bytes,mtime_ns=excluded.mtime_ns,
                status=excluded.status,updated_ms=excluded.updated_ms''',
                       (relative, 'camera', start, start + round(duration * 1000), duration,
                        after.st_size, after.st_mtime_ns, 'ready' if completed else 'recovered', int(time.time()*1000)))

    def receive(self, message):
        kind = message.get('kind')
        if kind in ('segment_open', 'segment_complete'):
            path = self.safe_file(message['path'])
            if kind == 'segment_complete':
                self.inspect(path, completed=True)
            else:
                with self.lock, self.connect() as db:
                    db.execute('''INSERT INTO recordings(path,channel,start_ms,status,updated_ms)
                        VALUES(?,?,?,'writing',?) ON CONFLICT(path) DO UPDATE SET status='writing' ''',
                               (str(path.relative_to(self.root)), 'camera', self.file_start(path), int(time.time()*1000)))
            return
        if message.get('status') != 'ok':
            return
        stamp = int(message.get('received_at_ms', 0))
        if abs(stamp-time.time()*1000) > 30000:
            return
        session = str(message['session'])[:100]
        generation = int(message['source_generation'])
        revision = int(message['revision'])
        if revision <= self.seen_revision.get(session, -1):
            return
        self.seen_revision = {session: revision}
        self.last_event = time.time()
        labels = {}
        for obj in message.get('objects', [])[:256]:
            class_id, confidence = int(obj['class_id']), float(obj['confidence'])
            if math.isfinite(confidence) and 0.25 <= confidence <= 1:
                if class_id not in labels or confidence > labels[class_id][1]:
                    labels[class_id] = (str(obj['label'])[:100], confidence)
        for class_id, (label, confidence) in labels.items():
            key = (session, generation, class_id)
            old = self.active.get(key)
            if old and 0 <= stamp-old['end_ms'] <= 3000:
                old['end_ms'] = stamp
                old['confidence'] = max(confidence, old['confidence'])
                old['sequence'] = int(message['sequence'])
            else:
                if old:
                    self.save_event(old)
                self.active[key] = dict(id=None, session=session, generation=generation, class_id=class_id,
                                        label=label, start_ms=stamp, end_ms=stamp, confidence=confidence,
                                        sequence=int(message['sequence']))
        for key, value in list(self.active.items()):
            if stamp-value['end_ms'] > 3000 or key[:2] != (session, generation):
                self.save_event(value)
                del self.active[key]
        if time.monotonic()-self.last_flush >= 1:
            for value in self.active.values():
                self.save_event(value)
            self.last_flush = time.monotonic()

    def save_event(self, event):
        with self.lock, self.connect() as db:
            if event['id'] is None:
                cursor = db.execute('''INSERT INTO events(session,generation,class_id,label,start_ms,end_ms,confidence,sequence)
                    VALUES(:session,:generation,:class_id,:label,:start_ms,:end_ms,:confidence,:sequence)''', event)
                event['id'] = cursor.lastrowid
            else:
                db.execute('UPDATE events SET end_ms=:end_ms,confidence=:confidence,sequence=:sequence WHERE id=:id', event)

    def reconcile(self):
        now = time.time()
        with self.lock, self.connect() as db:
            indexed = {r['path']: dict(r) for r in db.execute('SELECT * FROM recordings')}
        count = 0
        for path in sorted((self.root / 'camera').glob('*/*.mp4')):
            if self.stop.is_set():
                return
            try:
                path = self.safe_file(str(path))
                stat = path.stat()
                relative = str(path.relative_to(self.root))
                old = indexed.pop(relative, None)
                if old and old['status'] == 'deleting':
                    continue
                # 不读取仍在增长的录像。漏掉钩子或异常退出的尾段由稳定窗口恢复。
                if now-stat.st_mtime < 90 or (old and old['mtime_ns'] == stat.st_mtime_ns and old['status'] in ('ready','recovered','invalid')):
                    continue
                if count >= 16:
                    continue
                count += 1
                try:
                    self.inspect(path)
                except (ValueError, KeyError, subprocess.SubprocessError) as error:
                    LOG.warning('invalid recording %s: %s', path.name, error)
                    with self.lock, self.connect() as db:
                        db.execute('''INSERT INTO recordings(path,channel,start_ms,bytes,mtime_ns,status,updated_ms)
                            VALUES(?,?,?,?,?,'invalid',?) ON CONFLICT(path) DO UPDATE SET status='invalid',
                            bytes=excluded.bytes,mtime_ns=excluded.mtime_ns''',
                                   (relative,'camera',self.file_start(path),stat.st_size,stat.st_mtime_ns,int(now*1000)))
            except (OSError, ValueError):
                LOG.exception('cannot inspect recording %s', path)
        with self.lock, self.connect() as db:
            for row in indexed.values():
                # 不把本轮未探测的合法文件误标为缺失。
                if not self.safe_file(row['path']).exists():
                    db.execute("DELETE FROM recordings WHERE id=?", (row['id'],))
        self.cleanup()
        self.last_scan = time.time()

    def cleanup(self):
        cutoff = int((time.time()-self.config['days']*86400)*1000)
        with self.lock, self.connect() as db:
            rows = list(db.execute("SELECT * FROM recordings ORDER BY start_ms"))
        total = sum(row['bytes'] for row in rows)
        for row in rows:
            usage = shutil.disk_usage(self.root)
            over = self.config['max_bytes'] and total > self.config['max_bytes']
            if row['status'] == 'writing' or (row['status'] != 'deleting' and row['start_ms'] >= cutoff and usage.free >= self.config['reserve'] and not over):
                continue
            path = self.safe_file(row['path'])
            # 即使索引状态落后，也绝不删除最近仍在写入的文件。
            if path.exists() and time.time()-path.stat().st_mtime < 90:
                continue
            with self.lock, self.connect() as db:
                db.execute("UPDATE recordings SET status='deleting' WHERE id=?", (row['id'],))
            path.unlink(missing_ok=True)
            with self.lock, self.connect() as db:
                db.execute('DELETE FROM recordings WHERE id=?', (row['id'],))
            total -= row['bytes']
        with self.lock, self.connect() as db:
            db.execute('DELETE FROM events WHERE end_ms<?', (cutoff,))
        if shutil.disk_usage(self.root).free < self.config['reserve']:
            raise OSError('录像磁盘剩余空间不足，已无可清理的旧分段')

    def run(self, ipc):
        next_scan = 0
        while not self.stop.is_set():
            try:
                try:
                    packet = ipc.recv(65536)
                    self.receive(json.loads(packet))
                except socket.timeout:
                    pass
                except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError):
                    LOG.exception('recording notification failed')
                if time.monotonic() >= next_scan:
                    self.reconcile()
                    self.error = ''
                    next_scan = time.monotonic()+10
            except Exception as error:
                self.error = str(error)
                LOG.exception('recording maintenance failed')
                next_scan = time.monotonic()+10
                self.stop.wait(1)
        for event in self.active.values():
            try:
                self.save_event(event)
            except sqlite3.Error:
                LOG.exception('cannot flush event')

    def day_range(self, day):
        date = dt.date.fromisoformat(day)
        zone = ZoneInfo(self.config['timezone'])
        start = dt.datetime.combine(date, dt.time(), zone)
        end = dt.datetime.combine(date+dt.timedelta(days=1), dt.time(), zone)
        return round(start.timestamp()*1000), round(end.timestamp()*1000)

    def listing(self, day):
        start, end = self.day_range(day)
        with self.connect() as db:
            rows = [dict(r) for r in db.execute('''SELECT id,start_ms,end_ms,duration,bytes,status FROM recordings
                WHERE channel='camera' AND start_ms<? AND end_ms>? AND status IN ('ready','recovered')
                ORDER BY start_ms LIMIT 2000''', (end, start))]
            events = [dict(r) for r in db.execute('''SELECT id,class_id,label,start_ms,end_ms,confidence FROM events
                WHERE start_ms<? AND end_ms>=? ORDER BY start_ms LIMIT 2000''', (end, start))]
        return dict(date=day,start_ms=start,end_ms=end,recordings=rows,events=events,timezone=self.config['timezone'])

    def playback(self, stamp, duration):
        if not math.isfinite(duration) or duration <= 0 or duration > 300:
            raise ValueError('回放窗口需在 0～300 秒之间')
        with self.connect() as db:
            row = db.execute('''SELECT * FROM recordings WHERE channel='camera' AND status IN ('ready','recovered')
                AND start_ms<=? AND end_ms>? ORDER BY start_ms DESC LIMIT 1''', (stamp,stamp)).fetchone()
        if not row or not self.safe_file(row['path']).is_file():
            raise FileNotFoundError('所选时间没有已完成录像')
        # 每次请求限制在当前已完成文件内；前端在结束后按索引续播。
        length = min(duration,(row['end_ms']-stamp)/1000)
        query = urlencode(dict(path='camera',start=dt.datetime.fromtimestamp(stamp/1000,UTC).isoformat(),duration=f'{length:.3f}',format='mp4'))
        offset = max(0,(stamp-row['start_ms'])/1000)
        return dict(url='/recording-media/get?'+query,
                media_url=f'/api/recordings/{row["id"]}/media#t={offset:.3f}',
                start_ms=stamp,end_ms=stamp+round(length*1000),recording_id=row['id'])


class AdStore:
    MAX_BYTES = 512 * 1024 * 1024

    def __init__(self, root):
        self.root = root
        self.videos = root / 'videos'
        self.videos.mkdir(parents=True, exist_ok=True)
        self.db_path = root / 'index.sqlite3'
        self.playlist_path = root / 'playlist.json'
        self.mode_path = root / 'display-mode'
        self.lock = threading.RLock()
        with self.connect() as db:
            db.executescript('''
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS ads(
                    id TEXT PRIMARY KEY, filename TEXT NOT NULL UNIQUE,
                    original_name TEXT NOT NULL, duration REAL NOT NULL,
                    bytes INTEGER NOT NULL, sort_order INTEGER NOT NULL,
                    enabled INTEGER NOT NULL DEFAULT 1, created_ms INTEGER NOT NULL);
            ''')
        if not self.mode_path.exists():
            self._write_text(self.mode_path, 'ad')
        self._write_playlist()

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.db_path, timeout=5)
        db.row_factory = sqlite3.Row
        db.execute('PRAGMA busy_timeout=5000')
        try:
            with db:
                yield db
        finally:
            db.close()

    @staticmethod
    def _write_text(path, text):
        fd, name = tempfile.mkstemp(prefix='.', dir=path.parent)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as output:
                output.write(text)
                output.flush()
                os.fsync(output.fileno())
            os.chmod(name, 0o644)
            os.replace(name, path)
        finally:
            if os.path.exists(name):
                os.unlink(name)

    def _write_playlist(self):
        with self.lock, self.connect() as db:
            rows = db.execute('SELECT filename FROM ads WHERE enabled=1 ORDER BY sort_order,id').fetchall()
        self._write_text(self.playlist_path, json.dumps([row['filename'] for row in rows], ensure_ascii=False))

    def list(self):
        with self.lock, self.connect() as db:
            return [dict(row) for row in db.execute(
                'SELECT id,original_name,duration,bytes,sort_order,enabled,created_ms FROM ads ORDER BY sort_order,id')]

    def _row(self, identifier):
        if not uuid.UUID(identifier):
            raise ValueError('invalid advertisement id')
        with self.connect() as db:
            row = db.execute('SELECT * FROM ads WHERE id=?', (identifier,)).fetchone()
        if not row:
            raise FileNotFoundError('广告不存在')
        return row

    def path(self, row):
        path = (self.videos / row['filename']).resolve()
        if not path.is_relative_to(self.videos):
            raise ValueError('invalid advertisement path')
        return path

    def upload(self, filename, source):
        safe_name = Path(filename or '').name[:200]
        if Path(safe_name).suffix.lower() != '.mp4':
            raise ValueError('广告必须是 MP4 文件')
        identifier = str(uuid.uuid4())
        target = self.videos / f'{identifier}.mp4'
        size = 0
        try:
            with target.open('wb') as output:
                while True:
                    chunk = source.read(1024 * 1024)
                    if not chunk:
                        break
                    size += len(chunk)
                    if size > self.MAX_BYTES:
                        raise ValueError('广告文件不能超过 512 MiB')
                    output.write(chunk)
            probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                                    '-show_entries', 'stream=codec_name:format=duration', '-of', 'json', str(target)],
                                   capture_output=True, text=True, timeout=15, check=True)
            data = json.loads(probe.stdout)
            streams = data.get('streams', [])
            duration = float(data['format']['duration'])
            if not streams or streams[0].get('codec_name') != 'h264' or not math.isfinite(duration) or not 0 < duration <= 86400:
                raise ValueError('广告必须包含有效的 H.264 视频流')
            with self.lock, self.connect() as db:
                sort_order = db.execute('SELECT COALESCE(MAX(sort_order), -1)+1 FROM ads').fetchone()[0]
                db.execute('INSERT INTO ads VALUES(?,?,?,?,?,?,?,?)',
                           (identifier, target.name, safe_name, duration, size, sort_order, 1, int(time.time()*1000)))
            self._write_playlist()
            return next(item for item in self.list() if item['id'] == identifier)
        except Exception:
            target.unlink(missing_ok=True)
            raise

    def delete(self, identifier):
        row = self._row(identifier)
        with self.lock, self.connect() as db:
            db.execute('DELETE FROM ads WHERE id=?', (identifier,))
        self.path(row).unlink(missing_ok=True)
        self._write_playlist()

    def reorder(self, identifiers):
        if not isinstance(identifiers, list) or len({str(value) for value in identifiers}) != len(identifiers):
            raise ValueError('invalid advertisement order')
        rows = self.list()
        known = {row['id'] for row in rows}
        if set(identifiers) != known:
            raise ValueError('advertisement order must contain every id exactly once')
        with self.lock, self.connect() as db:
            for position, identifier in enumerate(identifiers):
                db.execute('UPDATE ads SET sort_order=? WHERE id=?', (position, identifier))
        self._write_playlist()

    def mode(self):
        value = self.mode_path.read_text(encoding='utf-8').strip()
        return value if value in ('ad', 'live') else 'ad'

    def set_mode(self, value):
        if value not in ('ad', 'live'):
            raise ValueError('mode must be ad or live')
        self._write_text(self.mode_path, value)
        return value


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        LOG.debug(format,*args)

    def do_GET(self):
        try:
            parsed = urlsplit(self.path)
            query = parse_qs(parsed.query)
            store = self.server.store
            ads = self.server.ads
            if parsed.path == '/api/recordings/status':
                usage = shutil.disk_usage(store.root)
                result = dict(status='degraded' if store.error else 'ok',detail=store.error,
                              root=str(store.root),free_bytes=usage.free,total_bytes=usage.total,
                              last_scan=store.last_scan,last_event=store.last_event)
            elif parsed.path == '/api/recordings':
                result = store.listing(query.get('date',[''])[0])
            elif parsed.path == '/api/ads':
                result = dict(ads=ads.list(), mode=ads.mode())
            elif parsed.path == '/api/display/mode':
                result = dict(mode=ads.mode())
            elif parsed.path == '/api/recordings/days':
                zone = ZoneInfo(store.config['timezone'])
                with store.connect() as db:
                    ranges = db.execute("SELECT start_ms,end_ms FROM recordings WHERE status IN ('ready','recovered')").fetchall()
                days = set()
                for start,end in ranges:
                    first = dt.datetime.fromtimestamp(start/1000,zone).date()
                    last = dt.datetime.fromtimestamp(max(start,end-1)/1000,zone).date()
                    while first<=last:
                        days.add(first.isoformat());first+=dt.timedelta(days=1)
                result = sorted(days,reverse=True)
            elif parsed.path == '/api/playback':
                result = store.playback(int(query['start'][0]),float(query.get('duration',['60'])[0]))
            elif parsed.path.startswith('/api/recordings/') and parsed.path.endswith('/media'):
                identifier = int(parsed.path.split('/')[3])
                with store.connect() as db:
                    row = db.execute("SELECT * FROM recordings WHERE id=? AND status IN ('ready','recovered')",(identifier,)).fetchone()
                if not row or not store.safe_file(row['path']).is_file():
                    raise FileNotFoundError('录像已删除')
                self.send_response(200)
                self.send_header('Content-Type','video/mp4')
                self.send_header('Cache-Control','no-store')
                self.send_header('X-Accel-Redirect','/_recordings/'+quote(row['path'],safe='/'))
                self.end_headers()
                return
            elif parsed.path.startswith('/api/recordings/') and parsed.path.endswith('/download'):
                identifier = int(parsed.path.split('/')[3])
                with store.connect() as db:
                    row = db.execute("SELECT * FROM recordings WHERE id=? AND status IN ('ready','recovered')",(identifier,)).fetchone()
                if not row or not store.safe_file(row['path']).is_file():
                    raise FileNotFoundError('录像已删除')
                self.send_response(200)
                self.send_header('Content-Type','video/mp4')
                self.send_header('Content-Disposition',f'attachment; filename="recording-{identifier}.mp4"')
                self.send_header('X-Accel-Redirect','/_recordings/'+quote(row['path'],safe='/'))
                self.end_headers()
                return
            elif parsed.path.startswith('/api/ads/') and parsed.path.endswith('/media'):
                identifier = parsed.path.split('/')[3]
                row = ads._row(identifier)
                if not ads.path(row).is_file():
                    raise FileNotFoundError('广告文件已删除')
                self.send_response(200)
                self.send_header('Content-Type', 'video/mp4')
                self.send_header('Cache-Control', 'no-store')
                self.send_header('X-Accel-Redirect', '/_ads/' + quote(row['filename']))
                self.end_headers()
                return
            else:
                raise FileNotFoundError('接口不存在')
            self.respond(200,result)
        except FileNotFoundError as error:
            self.respond(404,dict(error=str(error)))
        except (ValueError,KeyError,OverflowError) as error:
            self.respond(400,dict(error=str(error)))
        except (BrokenPipeError,ConnectionResetError):
            pass
        except Exception:
            LOG.exception('recording API failed')
            self.respond(503,dict(error='录像服务暂不可用'))

    def _json_body(self):
        length = int(self.headers.get('Content-Length', '0'))
        if length <= 0 or length > 1024 * 1024:
            raise ValueError('invalid request body')
        data = json.loads(self.rfile.read(length))
        if not isinstance(data, dict):
            raise ValueError('request body must be an object')
        return data

    def do_POST(self):
        try:
            if urlsplit(self.path).path != '/api/ads':
                raise FileNotFoundError('接口不存在')
            length = int(self.headers.get('Content-Length', '0'))
            if length <= 0 or length > AdStore.MAX_BYTES + 1024 * 1024:
                raise ValueError('广告文件大小无效')
            form = cgi.FieldStorage(fp=self.rfile, headers=self.headers,
                                    environ={'REQUEST_METHOD': 'POST', 'CONTENT_TYPE': self.headers.get('Content-Type', '')})
            field = form['video'] if 'video' in form else None
            if field is None or not getattr(field, 'filename', None):
                raise ValueError('缺少 video 文件字段')
            result = self.server.ads.upload(field.filename, field.file)
            self.respond(201, result)
        except FileNotFoundError as error:
            self.respond(404, dict(error=str(error)))
        except (ValueError, KeyError, OverflowError, TypeError) as error:
            self.respond(400, dict(error=str(error)))
        except Exception:
            LOG.exception('advertisement upload failed')
            self.respond(503, dict(error='广告上传失败'))

    def do_PUT(self):
        try:
            parsed = urlsplit(self.path)
            data = self._json_body()
            if parsed.path == '/api/ads/order':
                self.server.ads.reorder(data.get('ids'))
                result = dict(ads=self.server.ads.list())
            elif parsed.path == '/api/display/mode':
                result = dict(mode=self.server.ads.set_mode(data.get('mode')))
            else:
                raise FileNotFoundError('接口不存在')
            self.respond(200, result)
        except FileNotFoundError as error:
            self.respond(404, dict(error=str(error)))
        except (ValueError, KeyError, OverflowError, TypeError) as error:
            self.respond(400, dict(error=str(error)))
        except Exception:
            LOG.exception('advertisement control failed')
            self.respond(503, dict(error='广告控制暂不可用'))

    def do_DELETE(self):
        try:
            parsed = urlsplit(self.path)
            if not parsed.path.startswith('/api/ads/'):
                raise FileNotFoundError('接口不存在')
            self.server.ads.delete(parsed.path.split('/')[3])
            self.respond(200, dict(ok=True))
        except FileNotFoundError as error:
            self.respond(404, dict(error=str(error)))
        except (ValueError, KeyError, OverflowError, TypeError) as error:
            self.respond(400, dict(error=str(error)))
        except Exception:
            LOG.exception('advertisement deletion failed')
            self.respond(503, dict(error='广告删除失败'))

    def respond(self, status, payload):
        body = json.dumps(payload,ensure_ascii=False,allow_nan=False).encode()
        self.send_response(status)
        self.send_header('Content-Type','application/json; charset=utf-8')
        self.send_header('Cache-Control','no-store')
        self.send_header('Content-Length',str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 16
    def get_request(self):
        connection,address = super().get_request()
        connection.settimeout(10)
        return connection,address


def main():
    parser = argparse.ArgumentParser(description='录像索引和回放 API 服务')
    parser.add_argument('--config',required=True)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO,format='%(asctime)s %(levelname)s %(message)s')
    config = configuration(args.config)
    store = Store(config)
    ads = AdStore(config['ads_root'])
    ipc_path = Path(config['socket'])
    ipc_path.parent.mkdir(parents=True,exist_ok=True)
    ipc_path.unlink(missing_ok=True)
    ipc = socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
    ipc.bind(str(ipc_path));os.chmod(ipc_path,0o600);ipc.settimeout(.2)
    server = Server((config['host'],config['port']),Handler)
    server.store = store
    server.ads = ads
    worker = threading.Thread(target=store.run,args=(ipc,),name='recording-index')
    def stop(*_):
        store.stop.set()
    signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
    server.timeout=.5
    worker.start()
    LOG.info('recording root=%s listen=%s:%s',store.root,config['host'],config['port'])
    try:
        while not store.stop.is_set():
            server.handle_request()
    finally:
        store.stop.set();worker.join();server.server_close();ipc.close();ipc_path.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
