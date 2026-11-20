#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
极米 H3S v2 —— 电脑端「显示模式」接收程序（托盘版）
==================================================

它干什么
--------
在局域网里监听 UDP 54321，收到 ESP32 发来的指令后调用
Windows 自带的 DisplaySwitch.exe 切换显示模式。

    EXTEND    -> 扩展
    CLONE     -> 复制
    EXTERNAL  -> 仅投影（仅第二屏幕）
    INTERNAL  -> 仅电脑（仅主屏幕）
    PING      -> 回 PONG（自检用）

界面
----
右下角托盘区有一个小圆点图标：

    绿色 = 正常监听
    灰色 = 已暂停
    红色 = 出错（例如端口被占用）

右键菜单：暂停/恢复、打开日志、退出。双击图标 = 打开日志。

怎么跑
------
    pythonw display_switch.py     # 后台 + 托盘（日常用这个）
    python  display_switch.py     # 前台 + 托盘（调试用）

依赖
----
    pip install pystray pillow

    没装也能跑：会自动退回"只有日志文件、没有托盘"的模式。

开机自启
--------
任务计划程序 → 创建基本任务 → 触发器「当我登录时」→ 操作启动 pythonw.exe
    程序: pythonw.exe 的完整路径
    参数: "本文件的完整路径"
    起点: 本文件所在目录
关键：勾「只在用户登录时运行」——否则跑在服务会话里，
      DisplaySwitch 切的是看不见的桌面，你屏幕上毫无反应。

资源占用
--------
内存约 20~30 MB（多一个 Pillow 的图），空闲 CPU 约 0%。
"""

import os
import socket
import subprocess
import sys
import threading
import time

# ---------------------------------------------------------------- 配置

PORT = 54321
HOST = "0.0.0.0"

DISPLAY_SWITCH = r"C:\Windows\System32\DisplaySwitch.exe"

ACTIONS = {
    "EXTEND":   "/extend",     # 扩展
    "CLONE":    "/clone",      # 复制
    "EXTERNAL": "/external",   # 仅第二屏幕（投影）
    "INTERNAL": "/internal",   # 仅主屏幕（电脑）
}

# 同一条指令在这个时间内重复到达就忽略（ESP32 为了抗丢包会连发 2 次）
DEDUPE_SECONDS = 0.8

LOG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "display_switch.log")
LOG_MAX_BYTES = 1024 * 1024

COLOR_OK    = (46, 160, 90)
COLOR_PAUSE = (130, 130, 130)
COLOR_ERROR = (200, 60, 60)


# ---------------------------------------------------------------- 状态

class State:
    """UDP 线程和托盘共用的小状态盒子"""

    def __init__(self):
        self.lock = threading.Lock()
        self.count = 0          # 累计收到的指令数
        self.last = ""          # 最近一条指令
        self.last_at = 0.0      # 最近一条的时间
        self.paused = False
        self.error = ""         # 出错信息（空 = 正常）
        self.running = True
        self.sock = None

    def hit(self, cmd):
        with self.lock:
            self.count += 1
            self.last = cmd
            self.last_at = time.time()


STATE = State()


# ---------------------------------------------------------------- 日志

def log(msg):
    """写日志文件；有控制台时也打印出来。"""
    line = "%s  %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)

    if sys.stdout is not None:          # pythonw 下 stdout 是 None，不能 print
        try:
            print(line, flush=True)
        except Exception:
            pass

    try:
        if os.path.exists(LOG_PATH) and os.path.getsize(LOG_PATH) > LOG_MAX_BYTES:
            backup = LOG_PATH + ".1"
            if os.path.exists(backup):
                os.remove(backup)
            os.replace(LOG_PATH, backup)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


# ---------------------------------------------------------------- 执行

def apply_mode(action):
    """真正去切换显示模式。"""
    try:
        subprocess.run([DISPLAY_SWITCH, action], check=False, timeout=10)
        return True
    except FileNotFoundError:
        log("找不到 DisplaySwitch.exe，这台机器是不是 Windows？")
    except subprocess.TimeoutExpired:
        log("DisplaySwitch 执行超时（可能显卡驱动卡住了）")
    except Exception as e:      # noqa: BLE001 - 这里就是要兜住所有异常
        log("执行失败: %r" % (e,))
    return False


# ---------------------------------------------------------------- UDP 线程

def udp_loop():
    """在后台线程里收包"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        sock.bind((HOST, PORT))
    except OSError as e:
        STATE.error = "端口 %d 绑定失败：%s" % (PORT, e)
        log(STATE.error)
        log("可能是已经开了一份在跑，或者端口被别的程序占了。")
        return

    sock.settimeout(1.0)        # 每秒醒一次，方便检查 running / paused
    STATE.sock = sock
    STATE.error = ""
    log("已启动，监听 UDP %s:%d" % (HOST, PORT))
    log("支持指令: %s" % ", ".join(sorted(ACTIONS.keys())))

    last_cmd, last_at = "", 0.0

    while STATE.running:
        try:
            data, addr = sock.recvfrom(1024)
        except socket.timeout:
            continue
        except OSError:
            break

        cmd = data.decode("utf-8", "ignore").strip().upper()

        if cmd == "PING":
            try:
                sock.sendto(b"PONG", addr)
            except OSError:
                pass
            continue

        if STATE.paused:
            log("（已暂停）收到 %r 来自 %s，忽略" % (cmd, addr[0]))
            continue

        log("收到 %r 来自 %s" % (cmd, addr[0]))

        action = ACTIONS.get(cmd)
        if action is None:
            log("未知指令，忽略")
            continue

        # 去重：ESP32 会连发 2 个同样的包
        now = time.time()
        if cmd == last_cmd and (now - last_at) < DEDUPE_SECONDS:
            log("与上一条重复，忽略")
            continue
        last_cmd, last_at = cmd, now

        STATE.hit(cmd)
        if apply_mode(action):
            log("已执行 DisplaySwitch %s" % action)

    try:
        sock.close()
    except OSError:
        pass


# ---------------------------------------------------------------- 托盘

def _make_icon(color):
    """画一个纯色圆点当图标（不依赖外部图片文件）"""
    from PIL import Image, ImageDraw
    size = 64
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse((6, 6, size - 6, size - 6), fill=color + (255,))
    return img


def tray_main():
    import pystray
    from pystray import Menu, MenuItem

    icons = {
        "ok":    _make_icon(COLOR_OK),
        "pause": _make_icon(COLOR_PAUSE),
        "error": _make_icon(COLOR_ERROR),
    }

    def current_key():
        if STATE.error:
            return "error"
        if STATE.paused:
            return "pause"
        return "ok"

    def refresh():
        icon.icon = icons[current_key()]
        icon.title = "极米显示切换 · " + (
            ("出错：" + STATE.error) if STATE.error else
            ("已暂停" if STATE.paused else "监听中，已收 %d 次" % STATE.count)
        )
        try:
            icon.update_menu()
        except Exception:
            pass

    def on_toggle(icon_, item):
        STATE.paused = not STATE.paused
        log("托盘：%s" % ("已暂停" if STATE.paused else "已恢复"))
        refresh()

    def on_open_log(icon_, item):
        if os.path.exists(LOG_PATH):
            os.startfile(LOG_PATH)      # noqa: S606 - 就是打开记事本
        else:
            log("日志文件还不存在")

    def on_open_folder(icon_, item):
        os.startfile(os.path.dirname(LOG_PATH))    # noqa: S606

    def on_quit(icon_, item):
        log("托盘：退出")
        STATE.running = False
        try:
            if STATE.sock:
                STATE.sock.close()
        except OSError:
            pass
        icon.stop()

    def status_text(item):
        if STATE.error:
            return "状态：出错"
        return "状态：已暂停" if STATE.paused else "状态：监听中"

    def count_text(item):
        return "已接收：%d 次" % STATE.count

    def last_text(item):
        if not STATE.last:
            return "最近：—"
        return "最近：%s (%s)" % (
            STATE.last,
            time.strftime("%H:%M:%S", time.localtime(STATE.last_at)),
        )

    def error_text(item):
        return "错误：%s" % STATE.error[:40]

    menu = Menu(
        MenuItem(status_text, None, enabled=False),
        MenuItem(count_text, None, enabled=False),
        MenuItem(last_text, None, enabled=False),
        MenuItem(error_text, None, enabled=False,
                 visible=lambda i: bool(STATE.error)),
        Menu.SEPARATOR,
        MenuItem("暂停 / 恢复", on_toggle),
        MenuItem("打开日志", on_open_log, default=True),
        MenuItem("打开日志文件夹", on_open_folder),
        Menu.SEPARATOR,
        MenuItem("退出", on_quit),
    )

    icon = pystray.Icon("xgimi_display", icons[current_key()], "极米显示切换", menu)

    def ticker():
        """每 2 秒刷新一次图标和菜单，让计数/时间跟着变"""
        while STATE.running:
            time.sleep(2)
            try:
                refresh()
            except Exception:
                pass

    threading.Thread(target=udp_loop, daemon=True).start()
    threading.Thread(target=ticker, daemon=True).start()

    log("托盘已启动")
    icon.run()          # 阻塞在托盘消息循环里

    STATE.running = False


# ---------------------------------------------------------------- 兜底

def console_main(reason=""):
    """没装 pystray 时的退化模式：只有控制台 + 日志文件"""
    if reason:
        log("进入无托盘模式，原因: %s" % reason)
    log("想要托盘图标请执行:  python -m pip install pystray pillow")
    log("（用 python -m pip 能保证装进当前正在运行的这个 Python）")
    try:
        udp_loop()
    except KeyboardInterrupt:
        log("收到 Ctrl+C，退出")


# ---------------------------------------------------------------- 入口

def main():
    if not os.path.exists(DISPLAY_SWITCH):
        log("警告：%s 不存在，这个程序在这台机器上可能用不了" % DISPLAY_SWITCH)

    if os.name != "nt":
        log("注意：这不是 Windows，DisplaySwitch 不可用")

    # 把解释器路径写进日志 —— 排查"库装到别的 Python 里"时全靠这一行
    log("解释器: %s" % sys.executable)

    reason = ""
    try:
        import pystray  # noqa: F401
        from PIL import Image  # noqa: F401
        has_tray = True
    except ImportError as e:
        has_tray = False
        reason = "%r" % (e,)        # 显示真实原因（缺哪个模块）

    if has_tray:
        try:
            tray_main()
        except Exception as e:      # noqa: BLE001
            console_main("托盘初始化失败 %r" % (e,))
    else:
        console_main(reason)


if __name__ == "__main__":
    main()
