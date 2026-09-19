"""通过 Chromium DevTools 在板端验证真实回放页面，不依赖 npm。"""
import base64
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request


def verify(url):
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with tempfile.TemporaryDirectory(prefix='rkmon-chromium.') as profile:
        log=open(Path(profile)/'browser.log','w')
        browser=subprocess.Popen(['chromium-browser','--headless','--disable-gpu','--no-sandbox',
            '--disable-dev-shm-usage','--autoplay-policy=no-user-gesture-required',
            '--remote-debugging-port=19222','--user-data-dir='+profile,url],stdout=log,stderr=subprocess.STDOUT)
        connection=None
        try:
            target=None
            for _ in range(100):
                try:
                    with opener.open('http://127.0.0.1:19222/json',timeout=1) as r:
                        targets=json.load(r)
                    target=next((t for t in targets if t.get('type')=='page' and t.get('url')==url),None)
                    if target:break
                except OSError:pass
                time.sleep(.1)
            assert target,'Chromium page unavailable'
            path=target['webSocketDebuggerUrl'].split('19222',1)[1]
            connection=socket.create_connection(('127.0.0.1',19222),timeout=10)
            key=base64.b64encode(os.urandom(16)).decode()
            connection.sendall(f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1:19222\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n'.encode())
            header=b''
            while not header.endswith(b'\r\n\r\n'):header+=connection.recv(1)
            assert header.startswith(b'HTTP/1.1 101 '),header
            identifier=0
            def exact(size):
                result=b''
                while len(result)<size:
                    part=connection.recv(size-len(result))
                    if not part:raise EOFError('browser closed debugger')
                    result+=part
                return result
            def evaluate(expression):
                nonlocal identifier
                identifier+=1
                data=json.dumps(dict(id=identifier,method='Runtime.evaluate',params=dict(expression=expression,returnByValue=True))).encode()
                mask=os.urandom(4)
                frame=bytes([0x81,0x80|len(data)]) if len(data)<126 else bytes([0x81,0xfe])+struct.pack('!H',len(data))
                connection.sendall(frame+mask+bytes(b^mask[i%4] for i,b in enumerate(data)))
                while True:
                    head=exact(2);length=head[1]&127
                    if length==126:length=struct.unpack('!H',exact(2))[0]
                    if length==127:length=struct.unpack('!Q',exact(8))[0]
                    response=json.loads(exact(length))
                    if response.get('id')==identifier:
                        assert 'exceptionDetails' not in response.get('result',{}),response
                        return response.get('result',{}).get('result',{}).get('value')
            for _ in range(100):
                if evaluate("document.querySelectorAll('#recording-list button').length"):
                    break
                time.sleep(.1)
            assert evaluate("document.querySelectorAll('#recording-list button').length")>0,'recording list empty'
            assert evaluate("document.querySelectorAll('#event-list button').length")>0,'event list empty'
            evaluate("document.querySelector('#recording-list button').click()")
            for _ in range(100):
                state=evaluate("(()=>{const v=document.getElementById('recording-player');return {ready:v.readyState,time:v.currentTime,error:v.error?.message,message:document.getElementById('recording-message').textContent}})()")
                if state['ready']>=2 and state['time']>.1:break
                time.sleep(.1)
            assert state['ready']>=2 and state['time']>.1,state
            evaluate("document.getElementById('recording-speed').value='2';document.getElementById('recording-speed').dispatchEvent(new Event('change'))")
            assert evaluate("document.getElementById('recording-player').playbackRate")==2
            print('PASS: Chromium recording list, events, playable video, advancing clock and 2x playback',flush=True)
        finally:
            if connection:connection.close()
            browser.terminate()
            try:browser.wait(timeout=5)
            except subprocess.TimeoutExpired:browser.kill();browser.wait()
            log.close()
