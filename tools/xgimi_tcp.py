#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
极米 H3S TCP 13145 控制协议 —— 电脑端验证工具
================================================

协议格式（从手机 App 抓包还原）:

    ┌──────────────┬────────────────┬──────────────┐
    │ FF FF E0 4A  │   长度(4字节)   │  UTF-8 JSON  │
    │   固定魔数    │    大端序      │              │
    └──────────────┴────────────────┴──────────────┘

握手流程:
    1. 连上 投影IP:13145
    2. 发 action=10000（带 phoneName / phoneUid）
    3. 投影回 action=10001（设备信息）
    4. 之后才能发 action=20000 控制指令

用法:
    python xgimi_tcp.py 192.168.0.30 --probe           # 只握手，看设备信息
    python xgimi_tcp.py 192.168.0.30 --mode 7 --type 3 # 发一条控制指令
    python xgimi_tcp.py 192.168.0.30 --mode 7 --type 3 --listen 3   # 发完再等 3 秒收回复
"""

import argparse
import json
import socket
import struct
import sys
import time
import uuid

MAGIC = b"\xff\xff\xe0\x4a"
PORT = 13145


def build_frame(obj):
    """把 dict 打包成一帧：魔数 + 4 字节大端长度 + JSON"""
    data = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return MAGIC + struct.pack(">I", len(data)) + data


class XgimiClient:
    def __init__(self, host, port=PORT, timeout=5.0):
        self.host = host
        self.port = port
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def _read_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                return b""
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, obj):
        self.sock.sendall(build_frame(obj))

    def recv(self, timeout=5.0):
        """读一帧，返回 dict；超时返回 None"""
        self.sock.settimeout(timeout)
        try:
            header = self._read_exact(8)
        except socket.timeout:
            return None
        if len(header) < 8:
            return None
        if header[:4] != MAGIC:
            print("  [!] 魔数不对: %s" % header[:4].hex())
            return None
        size = struct.unpack(">I", header[4:8])[0]
        body = self._read_exact(size)
        if len(body) < size:
            return None
        try:
            return json.loads(body.decode("utf-8", "replace"))
        except Exception:
            print("  [!] JSON 解析失败: %r" % body[:200])
            return None

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def handshake(cli, host):
    """发 10000，收 10001"""
    req = {
        "action": 10000,
        "appid": "1",
        "authCode": {
            "authState": -1,
            "ip": host,
            "phoneName": "PC",
            "phoneUid": str(uuid.uuid4()),
            "timeOut": 15000,
        },
        "msgid": "1",
    }
    print("→ action=10000 (握手)")
    cli.send(req)

    resp = cli.recv(timeout=5)
    if resp is None:
        print("  [x] 没收到握手回复")
        return False

    action = resp.get("action")
    print("← action=%s" % action)
    info = resp.get("deviceInfo", {})
    if info:
        print("  设备: %s" % info.get("productName"))
        print("  型号: %s   固件: %s" % (info.get("device"), info.get("versioncode")))
        print("  音量: %s / %s" % (info.get("currentVolume"), info.get("maxVolume")))
    return action == 10001


def local_endpoint():
    """拿到本机在局域网里的 IP 和本次连接的本地端口（App 会把这个填进 10003）"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.0.30", 13145))
        ip = s.getsockname()[0]
    finally:
        s.close()
    return ip


def send_full_sequence(cli, host):
    """复现 App 的完整前置流程：10003 → 50022"""
    ip = local_endpoint()
    port = cli.sock.getsockname()[1]
    endpoint = "/%s:%d" % (ip, port)

    msg = {
        "action": 10003,
        "endScreenCapTime": 0,
        "msgid": "2",
        "startScreenCapTime": 0,
        "type": 0,
        "agreement": {
            "agreementType": 1,
            "isTcp": True,
            "msgId": "2",
            "phoneIp": endpoint,
            "port": PORT,
        },
    }
    print("→ action=10003 (注册控制端)  phoneIp=%s" % endpoint)
    cli.send(msg)

    msg50022 = {
        "action": 50022,
        "appid": None,
        "artMode": None,
        "authCode": None,
        "commonCmd": None,
        "controlCmd": None,
        "curtainAlignment": None,
        "customPlay": None,
        "exhibitionInfo": None,
        "hotSpot": None,
        "msgid": "700002",
        "packageName": None,
        "params": None,
        "phoneInfo": None,
        "photoWall": None,
        "statistics": None,
        "thirdPlay": None,
        "version": None,
        "wallFlatnessAdjustment": None,
    }
    print("→ action=50022 (查询设备能力)")
    cli.send(msg50022)

    # 把投影回来的东西都打出来
    for _ in range(6):
        resp = cli.recv(timeout=2.0)
        if resp is None:
            break
        print("← action=%s" % resp.get("action"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", help="投影 IP，例如 192.168.0.30")
    ap.add_argument("--mode", type=int, help="controlCmd.mode")
    ap.add_argument("--type", type=int, default=3, help="controlCmd.type（默认 3）")
    ap.add_argument("--time", type=int, default=0, help="controlCmd.time")
    ap.add_argument("--delay", type=int, default=0, help="controlCmd.delayTime")
    ap.add_argument("--probe", action="store_true", help="只握手，不发指令")
    ap.add_argument("--full", action="store_true", help="握手后再发 10003 / 50022 前置序列")
    ap.add_argument("--action", type=int, default=20000, help="直接指定 action 号（默认 20000）")
    ap.add_argument("--listen", type=float, default=2.0, help="发出后等几秒收回复")
    args = ap.parse_args()

    print("连接 %s:%d ..." % (args.host, PORT))
    try:
        cli = XgimiClient(args.host)
    except OSError as e:
        print("[x] 连不上: %s" % e)
        sys.exit(1)
    print("已连接\n")

    ok = handshake(cli, args.host)
    if not ok:
        cli.close()
        sys.exit(1)
    print("握手成功\n")

    if args.probe:
        cli.close()
        return

    if args.full:
        send_full_sequence(cli, args.host)
        print("")

    if args.mode is None:
        print("没给 --mode，只做握手")
        cli.close()
        return

    cmd = {
        "action": args.action,
        "controlCmd": {
            "delayTime": args.delay,
            "mode": args.mode,
            "time": args.time,
            "type": args.type,
        },
        "msgid": "2",
    }
    if args.action != 20000:
        cmd = {"action": args.action, "msgid": "2"}
    print("→ %s" % json.dumps(cmd, ensure_ascii=False))
    cli.send(cmd)

    deadline = time.time() + args.listen
    while time.time() < deadline:
        left = deadline - time.time()
        if left <= 0:
            break
        resp = cli.recv(timeout=left)
        if resp is None:
            break
        action = resp.get("action")
        print("← action=%s  %s" % (action, json.dumps(resp, ensure_ascii=False)[:300]))

    cli.close()
    print("\n完成。看投影有没有反应。")


if __name__ == "__main__":
    main()
