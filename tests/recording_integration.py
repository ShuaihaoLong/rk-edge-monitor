"""板端独立端口验证；读取现有 RTSP，不停止在线监控，不写正式录像目录。"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request

parser=argparse.ArgumentParser()
parser.add_argument('source',type=Path)
parser.add_argument('--mediamtx',default='/opt/rkmon/bin/mediamtx')
args=parser.parse_args()
root=Path(tempfile.mkdtemp(prefix='rkmon-recording-test.'))
print('test directory:',root,flush=True)
config=root/'recording.ini';sock=root/'events.sock'
config.write_text(f'[recording]\nroot={root}/video\nevent_socket={sock}\nport=19010\nreserve_mib=64\n')
media=root/'mediamtx.yml'
notify=args.source/'src/recording/notify.py'
media.write_text(f'''logLevel: warn
rtspAddress: 127.0.0.1:18554
rtspTransports: [tcp]
rtmp: false
hls: false
webrtc: false
srt: false
moq: false
playback: true
playbackAddress: 127.0.0.1:19996
paths:
  camera:
    source: rtsp://127.0.0.1:8554/camera
    rtspTransport: tcp
    record: true
    recordPath: {root}/video/%path/%Y-%m-%d/%H-%M-%S-%f
    recordFormat: fmp4
    recordPartDuration: 1s
    recordSegmentDuration: 3s
    recordDeleteAfter: 0s
    runOnRecordSegmentCreate: python3 {notify} segment_open {sock}
    runOnRecordSegmentComplete: python3 {notify} segment_complete {sock}
''')
processes=[];files=[]
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
def start(command,log,env=None):
    handle=(root/log).open('w');files.append(handle)
    proc=subprocess.Popen(command,stdout=handle,stderr=subprocess.STDOUT,env=env);processes.append(proc);return proc
def get(url):
    with opener.open(url,timeout=15) as result:return json.load(result)
def wait(callback,timeout=25):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        try:
            value=callback()
            if value:return value
        except (OSError,ValueError):pass
        time.sleep(.3)
    raise RuntimeError('timed out waiting for test condition')
try:
    service=start(['python3',str(args.source/'src/recording/service.py'),'--config',str(config)],'service.log')
    wait(lambda:sock.exists())
    recorder=start([args.mediamtx,str(media)],'mediamtx.log',dict(os.environ,TZ='UTC'))
    today=dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).date().isoformat()
    url='http://127.0.0.1:19010/api/recordings?date='+today
    listing=wait(lambda:(r if len((r:=get(url))['recordings'])>=2 else None))
    row=listing['recordings'][0]
    playback=get('http://127.0.0.1:19010/api/playback?start='+str(row['start_ms'])+'&duration=60')
    query=playback['url'].split('?',1)[1]
    with opener.open('http://127.0.0.1:19996/get?'+query,timeout=15) as response:
        (root/'playback.mp4').write_bytes(response.read())
    subprocess.run(['ffmpeg','-nostdin','-v','error','-i',str(root/'playback.mp4'),'-f','null','-'],check=True,timeout=15)
    stamp=int(time.time()*1000)
    with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as ipc:
        for i in range(3):
            ipc.sendto(json.dumps(dict(status='ok',session='integration',revision=i+1,source_generation=1,
                sequence=i,received_at_ms=stamp+i*100,objects=[dict(class_id=0,label='person',confidence=.9)])).encode(),str(sock))
            time.sleep(.1)
    wait(lambda:get(url)['events'])
    recorder.terminate();recorder.wait(timeout=10);time.sleep(1)
    service.terminate();service.wait(timeout=10)
    # 完成通知和重启幂等：保留数据启动新实例。
    service=start(['python3',str(args.source/'src/recording/service.py'),'--config',str(config)],'restart.log')
    recovered=wait(lambda:get(url)['recordings'])
    assert len(recovered)>=2 and get(url)['events']
    with opener.open('http://127.0.0.1:19010/api/recordings/'+str(row['id'])+'/download') as response:
        assert response.headers['X-Accel-Redirect'].startswith('/_recordings/camera/')
    # 独立 Nginx 验证同源接口、内部下载及浏览器页面，不变更线上站点。
    nginx=root/'nginx.conf'
    nginx.write_text(f'''pid {root}/nginx.pid;
error_log {root}/nginx.log;
events {{ worker_connections 64; }}
http {{ access_log off; include /etc/nginx/mime.types; server {{
 listen 127.0.0.1:19000;
 root {args.source}/web;
 location /api/ {{ proxy_pass http://127.0.0.1:19010; }}
 location = /recording-media/get {{ proxy_pass http://127.0.0.1:19996/get; }}
 location /_recordings/ {{ internal; alias {root}/video/; }}
 }} }}''')
    server=start(['nginx','-p',str(root),'-c',str(nginx),'-g','daemon off;'],'nginx-console.log')
    wait(lambda:get('http://127.0.0.1:19000/api/recordings/days'))
    with opener.open('http://127.0.0.1:19000/api/recordings/'+str(row['id'])+'/download') as response:
        assert response.headers['Content-Type']=='video/mp4'
        assert len(response.read())==row['bytes']
    recorder=start([args.mediamtx,str(media)],'playback-server.log',dict(os.environ,TZ='UTC'))
    time.sleep(1)
    from recording_browser import verify
    verify('http://127.0.0.1:19000/recordings.html')
    print('PASS: segmented recording, completion hooks, SQLite, HTTP playback decode, event aggregation, restart, Nginx download',flush=True)
finally:
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait()
    for handle in files:handle.close()
    for log in root.glob('*.log'):
        text=log.read_text()
        if text:print(log.name,text[-1800:])
