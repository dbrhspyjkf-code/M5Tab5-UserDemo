#!/usr/bin/env python3
"""tab5_serial_log.py — Tab5 串口日志守护 (重建版 2026-11-01, 2026-11-08 孤儿子进程修复)。

读 /dev/cu.usbmodem1301 (Tab5) 追加写 /tmp/radio-watch.log。
原脚本遗失后重建: 1300 秒空闲断线自动重连 (刷机时 esptool 抢占串口)。
看崩溃只看最后一个 "watcher start" 段 (追加写)。
⚠️ 刷机前清场用 `lsof -ti /dev/cu.usbmodem1301 | xargs kill` —— 本脚本是
subprocess 壳 + 裸读取子进程两层, pkill 只杀得掉壳, 孤儿子进程会一直占口
(esptool 报 "readiness to read but returned no data" 的真凶)。
"""
import subprocess
import sys
import time

PORT = "/dev/cu.usbmodem1301"
BAUD = 115200
LOG = "/tmp/radio-watch.log"


def one_round() -> float:
    with open(LOG, "ab", buffering=0) as f:
        f.write(f"\n===== watcher start {time.strftime('%F %T')} =====\n".encode())
    p = subprocess.Popen(
        ["python3", "-u", "-c", f"""
import os, serial, sys
try:
    s = serial.Serial('{PORT}', {BAUD}, timeout=2)
    while True:
        if os.getppid() == 1:
            sys.exit(0)  # 父壳被 pkill 后自己成了孤儿 → 退出让出串口, 否则 app-flash 必败
        b = s.read(4096)
        if not b:
            sys.stdout.flush()
            continue
        sys.stdout.buffer.write(b)
        sys.stdout.buffer.flush()
except Exception as e:
    sys.stderr.write(str(e))
    sys.exit(1)
"""],
        stdout=open(LOG, "ab", buffering=0),
        stderr=subprocess.DEVNULL,
    )
    t0 = time.time()
    while p.poll() is None and time.time() - t0 < 3600 * 6:
        time.sleep(1)   # 1s 轮询: 子进程异常退出即刻进入重连 (崩溃现场窗口最小化)
    p.terminate()
    p.wait()
    return time.time() - t0


if __name__ == "__main__":
    while True:
        try:
            dur = one_round()
            print(f"round ended after {dur:.0f}s, reconnecting…", file=sys.stderr)
        except Exception as e:
            print(f"round error: {e}", file=sys.stderr)
        time.sleep(0.2)   # 重连零延迟 (USB-Serial/JTAG 断连后立刻抢回)
