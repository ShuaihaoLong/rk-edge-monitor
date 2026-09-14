import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

binary = Path(sys.argv[1]).resolve()
launcher = Path(sys.argv[2]).resolve()

with tempfile.TemporaryDirectory(prefix="rkmon startup ") as temporary:
    root = Path(temporary)
    config_dir = root / "config files"
    config_dir.mkdir()
    ini = config_dir / "test.ini"
    ini.write_text("[logging]\nfile=logs/program.log\n[camera]\nenabled=false\n")
    for sig in (signal.SIGINT, signal.SIGTERM):
        output = root / "stdout.txt"
        with output.open("w") as stream:
            process = subprocess.Popen(
                ["bash", str(launcher), "--binary", str(binary), "--config", "config files/test.ini"],
                cwd=root, stdout=stream, stderr=subprocess.STDOUT,
            )
            try:
                deadline = time.monotonic() + 5
                while "services started" not in output.read_text():
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise AssertionError("application not ready: " + output.read_text())
                    time.sleep(0.01)
                assert Path(os.readlink(f"/proc/{process.pid}/exe")) == binary, "launcher did not exec"
                assert (config_dir / "logs/program.log").is_file(), "log path is not relative to INI"
                process.send_signal(sig)
                assert process.wait(timeout=3) == 0, output.read_text()
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
    for arguments in (["--help"], ["--target", "bad"], ["--config"], ["--binary", "missing"]):
        result = subprocess.run(["bash", str(launcher)] + arguments, cwd=root, capture_output=True, timeout=3)
        assert result.returncode == (0 if arguments == ["--help"] else (1 if arguments[0] == "--binary" else 2))
    # 摄像头缺失是可恢复状态，进程保持运行并等待设备出现。
    ini.write_text("[camera]\nenabled=true\ndevice=/dev/rkmon-missing-device\nreconnect_interval_ms=100\n")
    process = subprocess.Popen([str(binary), "--config", str(ini)], cwd=root,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        time.sleep(0.3)
        assert process.poll() is None, "missing camera stopped application"
        process.send_signal(signal.SIGTERM)
        output, _ = process.communicate(timeout=3)
        assert process.returncode == 0, output
        assert "offline" in output, output
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    for content, expected in (
        ("[camera]\nenabled=false\nfps=oops\n", "camera.fps"),
        ("[camera]\nenabld=true\n", "camera.enabld"),
    ):
        ini.write_text(content)
        result = subprocess.run([str(binary), "--config", str(ini)], cwd=root, capture_output=True, text=True, timeout=3)
        assert result.returncode == 1, result
        assert expected in result.stdout + result.stderr, result
    result = subprocess.run([str(binary), "--width", "640"], cwd=root, capture_output=True, timeout=3)
    assert result.returncode == 1, "device options remain in executable CLI"
print("startup and launcher tests passed")
