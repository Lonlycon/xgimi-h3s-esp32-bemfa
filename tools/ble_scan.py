#!/usr/bin/env python3
"""用电脑蓝牙扫描极米遥控器的 BLE 广播，抓开机 token（不用手机）。

依赖：
  pip install -U bleak

用法：
  1. 投影彻底断电（拔插头）
  2. python tools/ble_scan.py
  3. 对着投影连按遥控器电源键
  4. 看到 token 后填到固件的 BLE_TOKEN_HEX

如果 30 秒没扫到，再跑一次，或把遥控器贴近电脑。
"""

import asyncio
import sys

from bleak import BleakScanner

COMPANY_ID = 0x0046  # 极米


def on_detect(device, adv):
    name = adv.local_name or device.name or ""
    mfg = adv.manufacturer_data or {}

    if COMPANY_ID in mfg:
        data = mfg[COMPANY_ID]
        token = data[-16:]
        print("\n=== 找到极米遥控器 ===")
        print("name :", name)
        print("addr :", device.address)
        print("rssi :", adv.rssi)
        print("mfg  :", data.hex().upper())
        print("token:", token.hex().upper())
        print("（把 token 填到 BLE_TOKEN_HEX，共 32 个字符）\n")
    elif "bluetooth" in name.lower():
        print("候选:", name, device.address, "mfg=", mfg)


async def main():
    timeout = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    print(f"扫描 {timeout} 秒：请先拔掉投影电源，然后连按遥控器电源键...")
    scanner = BleakScanner(detection_callback=on_detect)
    await scanner.start()
    await asyncio.sleep(timeout)
    await scanner.stop()
    print("扫描结束。没找到就再跑一次，或把遥控器贴近电脑。")


if __name__ == "__main__":
    asyncio.run(main())
