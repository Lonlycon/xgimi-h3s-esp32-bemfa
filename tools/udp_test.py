#!/usr/bin/env python3
"""M0 协议验证：直接向极米投影发 UDP 指令（先用电脑验证，再烧 ESP32）。

用法：
  python udp_test.py 192.168.1.50 power     # 电源键（会出现关机确认）
  python udp_test.py 192.168.1.50 off       # 快速关机（推荐）
  python udp_test.py 192.168.1.50 vol+      # 音量加
  python udp_test.py 192.168.1.50 key 116   # 直接指定键值
"""

import socket
import sys

KEY = {
    "power": 116,
    "vol+": 115,
    "vol-": 114,
    "menu": 139,
    "back": 48,
    "play": 49,
    "up": 36,
    "down": 38,
    "left": 50,
    "right": 37,
    "home": 35,
}

FAST_OFF = (
    b'{"action":20000,"controlCmd":'
    b'{"delayTime":0,"mode":6,"time":0,"type":0},"msgid":"2"}'
)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return

    ip = sys.argv[1]
    cmd = sys.argv[2]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    if cmd == "off":
        s.sendto(FAST_OFF, (ip, 16750))
        print("sent fast-off -> %s:16750" % ip)
    elif cmd == "key" and len(sys.argv) > 3:
        code = int(sys.argv[3])
        s.sendto(("KEYPRESSES:%d" % code).encode(), (ip, 16735))
        print("sent KEYPRESSES:%d -> %s:16735" % (code, ip))
    elif cmd in KEY:
        s.sendto(("KEYPRESSES:%d" % KEY[cmd]).encode(), (ip, 16735))
        print("sent %s (KEYPRESSES:%d) -> %s:16735" % (cmd, KEY[cmd], ip))
    else:
        print("unknown command:", cmd)
        print(__doc__)


if __name__ == "__main__":
    main()
