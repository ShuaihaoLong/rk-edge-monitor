"""MediaMTX 分段钩子；索引服务不可用时由目录核对恢复，不能阻塞录制。"""
import json
import os
import socket
import sys

if __name__ == '__main__':
    try:
        with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as ipc:
            ipc.settimeout(.2)
            ipc.sendto(json.dumps(dict(kind=sys.argv[1],path=os.environ['MTX_SEGMENT_PATH'])).encode(),
                       sys.argv[2] if len(sys.argv)>2 else '/run/rkmon-recording/events.sock')
    except (OSError,KeyError,IndexError) as error:
        print('recording notification deferred to directory scan:',error,file=sys.stderr)
