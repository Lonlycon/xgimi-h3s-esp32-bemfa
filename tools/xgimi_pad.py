#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
极米 H3S TCP 13145 指令测试器（交互式 + 自动重连）
=================================================

连上投影一次，之后只要输入 "mode type" 就发一条指令，立刻看到回复。
连接被投影踢掉时会自动重连（某些无效指令会让投影关闭会话）。

协议:
    FF FF E0 4A | 4字节大端长度 | UTF-8 JSON

流程:
    10000 握手 -> 10001
    10003 注册 -> 10004
    50022 查能力 -> 50022
    20000 控制指令 -> 20001 / 30236 / 30412 / 100000(未知命令) / 无回复

用法:
    python xgimi_pad.py 192.168.0.30
    python xgimi_pad.py 192.168.0.30 --send 7 3
    python xgimi_pad.py 192.168.0.30 --scan 0 20 --ask

交互输入:
    7 3        发 mode=7 type=3
    7          只给 mode，type 用默认 3
    7 *        依次试 type 0..5
    scan 0 20  依次试 mode 0..20（跳过 6 关机）
    q          退出

乱码就先执行: chcp 65001
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
DEFAULT_TYPE = 3

# 从手机抓包里抄下来的身份。投影可能会"记住"认证过的 phoneUid，
# 用随机 UUID 冒充有可能只被授予只读权限（能查不能控）。
PHONE_UID = "b734a1ad-8865-4cfc-a65c-884efdaa8886"
PHONE_NAME = "小米App"

# 已知会关机/有破坏性的 mode，扫描时跳过
DANGEROUS_MODES = {6}

# 回复码 → 人话
REPLY_TABLE = {
    100000: "错误：未知命令（mode/type 不被接受）",
    20001:  "控制指令已受理",
    30236:  "返回 30236（带 imagePath，疑似截屏/取图）",
    30412:  "返回 30412（业务回复）",
    50022:  "设备能力信息",
}


def build_frame(obj):
    data = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return MAGIC + struct.pack(">I", len(data)) + data


class Projector:
    def __init__(self, host, port=PORT, timeout=6.0):
        self.host = host
        self.port = port
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    # ---------- 底层 ----------
    def _read_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("投影关闭了连接")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, obj):
        self.sock.sendall(build_frame(obj))

    def recv(self, timeout=2.0):
        self.sock.settimeout(timeout)
        try:
            header = self._read_exact(8)
        except socket.timeout:
            return None
        if header[:4] != MAGIC:
            raise ValueError("魔数错误: %s" % header[:4].hex())
        size = struct.unpack(">I", header[4:8])[0]
        body = self._read_exact(size)
        return json.loads(body.decode("utf-8", "replace"))

    def drain(self, seconds=2.0):
        out = []
        end = time.time() + seconds
        while True:
            left = end - time.time()
            if left <= 0:
                break
            try:
                msg = self.recv(timeout=left)
            except socket.timeout:
                break
            if msg is None:
                break
            out.append(msg)
            end = time.time() + 0.35
        return out

    # ---------- 流程 ----------
    def handshake(self, phone_uid=None, phone_name=None):
        self.send({
            "action": 10000,
            "appid": "1",
            "authCode": {
                "authState": -1,
                "ip": self.host,
                "phoneName": phone_name or PHONE_NAME,
                "phoneUid": phone_uid or PHONE_UID,
                "timeOut": 15000,
            },
            "msgid": "1",
        })
        msg = self.recv(timeout=6)
        if not msg or msg.get("action") != 10001:
            raise RuntimeError("握手失败: %r" % (msg,))
        return msg.get("deviceInfo", {})

    def register(self):
        me = self.sock.getsockname()
        self.send({
            "action": 10003,
            "endScreenCapTime": 0,
            "msgid": "2",
            "startScreenCapTime": 0,
            "type": 0,
            "agreement": {
                "agreementType": 1,
                "isTcp": True,
                "msgId": "2",
                "phoneIp": "/%s:%d" % (me[0], me[1]),
                "port": PORT,
            },
        })
        msg = self.recv(timeout=4)
        return msg.get("action") if msg else None

    def query_caps(self):
        self.send({
            "action": 50022, "appid": None, "artMode": None, "authCode": None,
            "commonCmd": None, "controlCmd": None, "curtainAlignment": None,
            "customPlay": None, "exhibitionInfo": None, "hotSpot": None,
            "msgid": "700002", "packageName": None, "params": None,
            "phoneInfo": None, "photoWall": None, "statistics": None,
            "thirdPlay": None, "version": None, "wallFlatnessAdjustment": None,
        })
        return self.recv(timeout=4)

    def command(self, mode, cmd_type, time_v=0, delay=0):
        self.send({
            "action": 20000,
            "controlCmd": {
                "delayTime": delay,
                "mode": mode,
                "time": time_v,
                "type": cmd_type,
            },
            "msgid": "2",
        })
        return self.drain(seconds=2.0)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def connect_ready(host, quiet=False):
    proj = Projector(host)
    info = proj.handshake()
    reg = proj.register()
    caps = proj.query_caps()
    if not quiet:
        print("[OK] handshake device=%s fw=%s vol=%s/%s" % (
            info.get("productName"), info.get("versioncode"),
            info.get("currentVolume"), info.get("maxVolume")), flush=True)
        print("[OK] register -> action=%s" % reg, flush=True)
        print("[OK] capabilities -> action=%s" % (caps.get("action") if caps else None),
              flush=True)
    return proj


def describe(msg):
    action = msg.get("action")
    text = REPLY_TABLE.get(action, "")
    extra = msg.get("extraInfo")
    if isinstance(extra, dict) and extra.get("message"):
        text = (text + " / " if text else "") + str(extra["message"])
    return "action=%s  %s" % (action, text)


class Session:
    """持有连接，断了自动重连"""

    def __init__(self, host):
        self.host = host
        self.proj = None

    def ensure(self):
        if self.proj is not None:
            return self.proj
        self.proj = connect_ready(self.host, quiet=True)
        return self.proj

    def drop(self):
        if self.proj:
            self.proj.close()
        self.proj = None

    def send_obj(self, obj):
        """发送任意 JSON 帧（带断线重连）"""
        for attempt in (1, 2):
            proj = self.ensure()
            try:
                proj.send(obj)
                return proj.drain(seconds=2.0)
            except (ConnectionError, OSError, ValueError) as e:
                if attempt == 2:
                    print("   !! 重连后仍然失败: %s" % type(e).__name__, flush=True)
                    return None
                print("   !! 连接被断开(%s)，正在重连..." % type(e).__name__, flush=True)
                self.drop()
                time.sleep(0.8)
        return None

    def send(self, mode, cmd_type):
        for attempt in (1, 2):
            proj = self.ensure()
            try:
                return proj.command(mode, cmd_type)
            except (ConnectionError, OSError, ValueError) as e:
                if attempt == 2:
                    print("   !! 重连后仍然失败: %s" % type(e).__name__, flush=True)
                    return None
                print("   !! 连接被断开(%s)，正在重连..." % type(e).__name__, flush=True)
                self.drop()
                time.sleep(0.8)
        return None

    def close(self):
        self.drop()


def show(mode, cmd_type, replies):
    print("-> mode=%s type=%s" % (mode, cmd_type), flush=True)
    if not replies:
        print("   (无回复)  <- 可能执行了但没回包，也可能没理", flush=True)
        return
    for r in replies:
        print("   <- %s" % describe(r), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="192.168.0.30")
    ap.add_argument("--send", nargs="+", type=int, metavar="N",
                    help="非交互：--send <mode> [type]")
    ap.add_argument("--scan", nargs=2, type=int, metavar=("FROM", "TO"),
                    help="非交互：扫描 mode 区间")
    ap.add_argument("--type", dest="scan_type", type=int, default=DEFAULT_TYPE)
    ap.add_argument("--gap", type=float, default=1.5)
    ap.add_argument("--ask", action="store_true", help="扫描时每条都问「有反应吗」")
    ap.add_argument("--raw", help="原样发送一段 JSON，例如 --raw '{\"action\":50036,...}'")
    ap.add_argument("--inst", type=int, metavar="CODE",
                    help="发送 50036/commonCmd 指令，--inst 12 表示 code=12")
    ap.add_argument("--inst-action", default="3", help="commonCmd.action（默认 3）")
    ap.add_argument("--inst-type", default="inst", help="commonCmd.type（默认 inst）")
    args = ap.parse_args()

    print("connecting %s:%d ..." % (args.host, PORT), flush=True)
    sess = Session(args.host)
    try:
        sess.ensure()
    except OSError as e:
        print("!! 连不上: %s" % e)
        sys.exit(1)
    print("[OK] 已就绪\n", flush=True)

    if args.send:
        mode = args.send[0]
        ctype = args.send[1] if len(args.send) > 1 else DEFAULT_TYPE
        show(mode, ctype, sess.send(mode, ctype))
        sess.close()
        return

    if args.raw:
        try:
            obj = json.loads(args.raw)
        except json.JSONDecodeError as e:
            print("!! JSON 格式错误: %s" % e)
            sess.close()
            sys.exit(1)
        print("-> %s" % json.dumps(obj, ensure_ascii=False), flush=True)
        replies = sess.send_obj(obj)
        if not replies:
            print("   (无回复)", flush=True)
        else:
            for r in replies:
                print("   <- %s" % describe(r), flush=True)
        sess.close()
        return

    if args.inst is not None:
        obj = {
            "action": 50036,
            "commonCmd": {
                "action": args.inst_action,
                "code": str(args.inst),
                "type": args.inst_type,
            },
        }
        print("-> %s" % json.dumps(obj, ensure_ascii=False), flush=True)
        replies = sess.send_obj(obj)
        if not replies:
            print("   (无回复)", flush=True)
        else:
            for r in replies:
                print("   <- %s" % describe(r), flush=True)
        sess.close()
        return

    if args.scan:
        lo, hi = args.scan
        for m in range(lo, hi + 1):
            if m in DANGEROUS_MODES:
                print("-- skip mode=%d (power off)" % m, flush=True)
                continue
            print("")
            show(m, args.scan_type, sess.send(m, args.scan_type))
            if args.ask:
                try:
                    ans = input("   >>> 有反应吗? y=停/q=退出/回车=继续 ").strip().lower()
                except (EOFError, KeyboardInterrupt):
                    break
                if ans.startswith("y"):
                    print("\n*** 找到: mode=%d type=%d ***" % (m, args.scan_type), flush=True)
                    break
                if ans.startswith("q"):
                    break
            else:
                time.sleep(args.gap)
        sess.close()
        return

    print("=== ready ===")
    print("  '7 3'       send mode=7 type=3")
    print("  '7'         mode=7, type=%d" % DEFAULT_TYPE)
    print("  '7 *'       try type 0..5")
    print("  'i 12'      send 50036 commonCmd code=12")
    print("  '{...}'     send raw JSON frame")
    print("  'scan 0 20' try mode 0..20 (skip 6)")
    print("  'q'         quit\n")

    while True:
        try:
            line = input("mode type> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not line:
            continue
        if line.lower() in ("q", "quit", "exit"):
            break

        parts = line.replace(",", " ").split()

        # 直接发一段 JSON
        if line.startswith("{"):
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as e:
                print("  JSON 错误: %s" % e)
                continue
            print("-> %s" % json.dumps(obj, ensure_ascii=False), flush=True)
            reps = sess.send_obj(obj)
            if not reps:
                print("   (无回复)", flush=True)
            else:
                for r in reps:
                    print("   <- %s" % describe(r), flush=True)
            continue

        # i 12  -> 发 50036/commonCmd{code:12}
        if parts[0].lower() in ("i", "inst") and len(parts) >= 2:
            obj = {
                "action": 50036,
                "commonCmd": {"action": "3", "code": str(parts[1]), "type": "inst"},
            }
            print("-> %s" % json.dumps(obj, ensure_ascii=False), flush=True)
            reps = sess.send_obj(obj)
            if not reps:
                print("   (无回复)", flush=True)
            else:
                for r in reps:
                    print("   <- %s" % describe(r), flush=True)
            continue

        if parts[0].lower() == "scan" and len(parts) >= 3:
            lo, hi = int(parts[1]), int(parts[2])
            for m in range(lo, hi + 1):
                if m in DANGEROUS_MODES:
                    print("-- skip mode=%d" % m, flush=True)
                    continue
                print("")
                show(m, DEFAULT_TYPE, sess.send(m, DEFAULT_TYPE))
                time.sleep(1.2)
            continue

        try:
            mode = int(parts[0])
        except ValueError:
            print("  请输入数字，例如: 7 3")
            continue

        if len(parts) >= 2 and parts[1] == "*":
            for t in range(0, 6):
                show(mode, t, sess.send(mode, t))
                time.sleep(1.0)
        else:
            ctype = int(parts[1]) if len(parts) >= 2 else DEFAULT_TYPE
            show(mode, ctype, sess.send(mode, ctype))

    sess.close()
    print("bye")


if __name__ == "__main__":
    main()
