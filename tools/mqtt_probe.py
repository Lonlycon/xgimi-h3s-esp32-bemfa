#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MQTT 订阅探针 —— 诊断"某个巴法云主题订阅不上"的问题。

两种模式：
  默认        一次 SUBSCRIBE 里带全部主题（一个个 filter 列表）
  --one       每个主题单独发一个 SUBSCRIBE 包（模拟 ESP32 的连发行为）
  --delay=毫秒 每个包之间等多久（配合 --one）

用法：
    python tools/mqtt_probe.py <私钥> jinzhu001 jinpc001
    python tools/mqtt_probe.py <私钥> --one --delay=200 touying001 kuozhan001 fuzhi001 jinzhu001 jinpc001

返回码：0x00/0x01 = 成功，0x80 = 服务器拒绝，-- = 超时没回应（等于被丢了）

注意：同一个私钥同时只能有一个连接，跑的时候 ESP32 会被顶下线，
      5 秒内它自己会重连并把你顶掉，所以脚本要在几秒内跑完。
"""

import socket
import struct
import sys
import time

HOST = "bemfa.com"
PORT = 9501


def enc_len(n):
    out = b""
    while True:
        b = n % 128
        n //= 128
        if n > 0:
            b |= 0x80
        out += bytes([b])
        if n == 0:
            return out


def enc_str(s):
    b = s.encode("utf-8")
    return struct.pack(">H", len(b)) + b


def read_packet(sock):
    hdr = sock.recv(1)
    if not hdr:
        return None, b""
    mult, length = 1, 0
    while True:
        b = sock.recv(1)
        if not b:
            return None, b""
        length += (b[0] & 0x7F) * mult
        if not (b[0] & 0x80):
            break
        mult *= 128
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            break
        body += chunk
    return hdr[0], body


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        print(__doc__)
        return
    one_mode = "--one" in sys.argv
    delay_ms = 0
    for a in sys.argv[1:]:
        if a.startswith("--delay="):
            delay_ms = int(a.split("=")[1])

    uid, topics = args[0], args[1:]

    sock = socket.create_connection((HOST, PORT), timeout=6)

    vh = enc_str("MQTT") + bytes([4, 0x02]) + struct.pack(">H", 60)
    body = vh + enc_str(uid)
    sock.sendall(bytes([0x10]) + enc_len(len(body)) + body)

    ptype, pbody = read_packet(sock)
    if ptype != 0x20:
        print("连接失败（无 CONNACK）")
        return
    print("CONNECT 返回码 %d" % pbody[1])
    if pbody[1] != 0:
        return

    print("模式：%s%s" % ("逐个发送" if one_mode else "一次性发送",
                        ("，间隔 %dms" % delay_ms) if delay_ms else ""))

    if one_mode:
        sock.settimeout(max(1.5, delay_ms / 1000.0 + 1.0))
        for i, t in enumerate(topics, start=1):
            pl = enc_str(t) + bytes([0x00])
            body = struct.pack(">H", i) + pl
            sock.sendall(bytes([0x82]) + enc_len(len(body)) + body)
            try:
                ptype, pbody = read_packet(sock)
            except socket.timeout:
                print("  %-12s -> -- （2 秒内没有 SUBACK，说明被服务器丢了）" % t)
                continue
            if ptype != 0x90:
                print("  %-12s -> 意外报文 0x%02X" % (t, ptype or 0))
                continue
            code = pbody[2] if len(pbody) >= 3 else -1
            print("  %-12s -> 0x%02X %s" % (t, code, "成功" if code in (0, 1) else "失败"))
            if delay_ms:
                time.sleep(delay_ms / 1000.0)
    else:
        sub = b""
        for t in topics:
            sub += enc_str(t) + bytes([0x00])
        body = struct.pack(">H", 1) + sub
        sock.sendall(bytes([0x82]) + enc_len(len(body)) + body)
        sock.settimeout(5)
        try:
            ptype, pbody = read_packet(sock)
        except socket.timeout:
            print("  没有 SUBACK")
            return
        codes = list(pbody[2:])
        for i, t in enumerate(topics):
            c = codes[i] if i < len(codes) else -1
            print("  %-12s -> 0x%02X %s" % (t, c, "成功" if c in (0, 1) else "失败"))

    sock.close()
    print("\n已断开，ESP32 会在 5 秒内重连。")


if __name__ == "__main__":
    main()
