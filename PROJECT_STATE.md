# 项目状态（PROJECT_STATE）

> 更新时间：2026-11-19
> 当前版本：v1.0（本地链路、巴法云、小爱语音已验证）
> 一句话：ESP32 模拟极米 H3S 蓝牙遥控器开机 + 局域网 UDP 关机，通过巴法云接入小爱。

---

## 1. 目标与边界

**目标**

- 用 ESP32 代替 H3S 遥控器，实现“打开/关闭投影仪”；
- 通过巴法云接入小爱，能用语音控制；
- 尽量不依赖额外硬件（不需要红外、不需要智能插座）。

**不做**

- 完整方向键/音量遥控器 UI（后续可加网页遥控）；
- 米家原生“投影仪”品类卡片（需要小米 IoT 开放平台 + 认证模组，DIY 不现实）。

---

## 2. 当前环境

| 项 | 值 |
|---|---|
| 开发板 | ESP32-D0WD-V3（CH340，COM3） |
| 板子 MAC | 见设备管理器/路由器（不写死） |
| 供电 | 独立 5V 常电（不要插投影 USB） |
| 投影 | 极米 H3S，IP `192.168.1.50`（示例） |
| 路由器网关 | `192.168.1.1`（示例） |
| Wi-Fi | `YOUR_WIFI_SSID`（2.4GHz，WPA2） |
| ESP32 IP | 由 DHCP 分配（示例 `192.168.1.177`） |
| 巴法云主题 | `touying001`（001 = 插座） |
| 巴法云设备名 | “插座”（可在巴法控制台改成“投影仪”） |
| Arduino-ESP32 | 3.3.13 |
| PubSubClient | 2.8 |
| arduino-cli | 1.6.0（工程内 `tools/arduino-cli`） |
| Python / bleak | 3.12 / 3.1.0（抓 token 用） |

> Wi-Fi 密码、巴法私钥、BLE token 在 `secrets.h` 里（已 gitignore），不在本文档重复。备份/分享前先脱敏。

---

## 3. 已实现功能

| 功能 | 状态 | 说明 |
|---|---|---|
| BLE 广播开机 | ✅ | `w` 验证过，`o` 完整流程成功 |
| UDP 快速关机 | ✅ | `f` 验证过，`[off ok]` |
| 状态检测 | ✅ | 8080 端口优先，554/5555/6466 兜底 |
| Wi-Fi 连接 | ✅ | 2.4GHz Wi-Fi，IP 由 DHCP 分配 |
| 巴法云 MQTT | ✅ | `touying001`，online=true |
| 小爱语音 | ✅ | 手机小爱可用；没有小爱音箱 |
| 米家设备卡片 | ❌ | 巴法“其他平台设备”通常不生成普通卡片，001/006 都一样 |
| Arduino IDE | ❌ | 当前用 arduino-cli 编译烧录；IDE 未安装 |
| OTA | ❌ | 未实现 |

---

## 4. 架构

```text
手机小爱 / 小爱音箱
        │
   米家 / 巴法云（bemfa.com:9501）
        │  MQTT  on / off
        ▼
   ESP32（Wi-Fi + BLE）
        │
        ├── BLE 广播（31 字节，token）──> H3S 开机
        ├── UDP 16750（快速关机 JSON）──> H3S 关机
        └── TCP 8080 轮询 ─────────────> 判断开机/关机
```

串口调试不经过云：

```text
COM3 115200  ──  w / o / f / s  ──> ESP32
```

---

## 5. 关键参数（不要随意改）

| 参数/代码 | 当前值 | 原因 |
|---|---|---|
| `BLE_ADV_MS` | `10000` | 5 秒不够稳，10 秒开机成功率更高 |
| `BOOT_TIMEOUT_MS` | `75000` | H3S 开机较慢，留足时间 |
| `POLL_MS` | `8000` | 状态轮询间隔 |
| `STATE_PORTS` | `{ 8080, 554, 5555, 6466 }` | H3S 实测 554 关闭、8080 开着 |
| `UDP_KEY_PORT` | `16735` | 极米简单按键协议 |
| `UDP_CMD_PORT` | `16750` | 极米复杂指令（快速关机） |
| MQTT | `bemfa.com:9501` | 巴法云 MQTT |
| `BEMFA_TOPIC` | `touying001` | 必须和巴法控制台一致 |
| `bleInUse()` | 返回 `true` | 防止 Arduino 核心释放 BLE 内存 |
| `cfg.mode` | `ESP_BT_MODE_BLE` | 经典蓝牙内存已被核心释放，只能 BLE-only |
| 分区 | `huge_app` | 固件 1.64MB，默认 1.3MB 装不下 |
| 上传速度 | `460800` | 921600 在 CH340 上到 96% 会掉线 |

---

## 6. 构建与烧录

### 6.1 arduino-cli（当前使用）

```powershell
$cli = "D:\demo\DSH\DSP32test\tools\arduino-cli\arduino-cli.exe"

& $cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" `
  "D:\demo\DSH\DSP32test\xgimi_mijia"

& $cli upload -p COM3 `
  --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" `
  --upload-property upload.speed=460800 `
  "D:\demo\DSH\DSP32test\xgimi_mijia"
```

### 6.2 Arduino IDE（如果以后安装）

- 开发板：**ESP32 Dev Module**
- 分区：**Huge APP (3MB No OTA/1MB SPIFFS)**
- 上传速度：**460800**
- 串口：**115200**

### 6.3 依赖位置

- 核心/工具链：`%LOCALAPPDATA%\Arduino15`（约 2GB）
- 编译缓存：`%LOCALAPPDATA%\arduino\sketches\...`
- 离线包：`tools\esp32-offline`（1.2GB，可选）
- arduino-cli：`tools\arduino-cli\arduino-cli.exe`

---

## 7. 验证步骤

### 7.1 串口命令

| 命令 | 预期 |
|---|---|
| `w` | `[BLE] 正在发送开机广播...`，10 秒后 `[BLE] stop`；投影应开机 |
| `o` | `[CMD] power on` → `[BLE] ...` → `[boot ok] state=3` |
| `f` | `[CMD] power off` → `[off ok] state=1` |
| `s` | 打印 `state=`、`ip=`、`mqtt=`、`ble=`、`adv=` |

### 7.2 巴法云

```text
http://apis.bemfa.com/vb/api/v2/topicInfo?openID=<私钥>&type=1&topic=touying001
```

应返回：

```json
{
  "online": true,
  "msg": "on/off",
  "deviceType": "outlet"
}
```

### 7.3 小爱

- 手机小爱：“打开插座 / 关闭插座”（设备名可在巴法控制台改成“投影仪”）。
- 小爱音箱：同上。

---

## 8. 已知问题与限制

1. **米家不显示卡片**
   巴法属于“其他平台设备”，米家通常只把它作为小爱/场景可用设备，不生成普通设备卡片；001 插座和 006 开关都试过，结果一样。不是固件问题。

2. **没有小爱音箱**
   手机小爱可用；米家“手动场景 → 小爱音箱 → 自定义指令”做不了，因为米家里没有音箱设备。替代方案：手机小爱快捷指令、HTTP 快捷键 App。

3. **token 可能滚动**
   BLE token 前几个字节可能随遥控器广播递增。开机失败先重新抓 token：
   `python tools/ble_scan.py`。

4. **H3S 的 554 端口关闭**
   不要改回只轮询 554；8080 是当前验证可用的状态端口。

5. **首次状态可能短暂显示 OFF**
   ESP32 启动时 8080 可能还没开，第一次轮询会判 OFF；下一次轮询会纠正为 ON。可接受。

6. **巴法 MQTT 会回显消息**
   启动时会看到 `[MQTT] touying001 -> off`，是云端的回显/保留消息；状态机是幂等的，不会误动作。

7. **敏感信息在 `.ino` 里**
   Wi-Fi 密码、巴法私钥、BLE token 都是明文宏。备份到公开仓库前必须脱敏。

---

## 9. 文件清单

| 文件 | 作用 |
|---|---|
| `xgimi_mijia.ino` | 固件主程序 |
| `README.md` | 使用/烧录/接入/排错 |
| `PROJECT_STATE.md` | 本文件，交接用 |
| `CHANGELOG.md` | 版本变更 |
| `tools/ble_scan.py` | 电脑蓝牙抓 token |
| `tools/udp_test.py` | 电脑直接测 UDP 开关机 |
| `tools/download_esp32_offline.ps1` | 下载 ESP32 离线包 |
| `tools/esp32-offline-urls.txt` | 18 个离线包 URL 清单 |
| `tools/arduino-cli/` | 便携 arduino-cli |
| `tools/esp32-offline/` | 离线包（1.2GB，可不备份） |

---

## 10. 备份与恢复

### 10.1 推荐

```bash
cd D:\demo\DSH\DSP32test\xgimi_mijia
git init
git add .
git commit -m "v1.0 working: BLE wake + UDP off + bemfa"
git tag v1.0
```

仓库保持**私有**；或把敏感宏抽到 `secrets.h` 并 `.gitignore`。

### 10.2 简单备份

```powershell
Compress-Archive -Path xgimi_mijia, tools\*.ps1, tools\*.txt `
  -DestinationPath D:\backup\xgimi-mijia-v1.0.zip
```

至少留两份，放另一块硬盘/网盘/U 盘。

### 10.3 整片 Flash 备份（可选，最保险）

```powershell
esptool.exe --port COM3 read_flash 0 0x400000 xgimi-v1.0-flash.bin
```

恢复：

```powershell
esptool.exe --port COM3 write_flash 0 xgimi-v1.0-flash.bin
```

该文件包含 Wi-Fi/私钥，**不要外传**。

---

## 11. 后续开发建议

| 功能 | 说明 |
|---|---|
| 网页遥控 | ESP32 起 WebServer，手机浏览器控方向键/音量/主页 |
| 配网页面 | WiFiManager/AP 配网，免改代码换 Wi-Fi |
| OTA | ArduinoOTA/HTTP OTA，免插线升级 |
| 人体传感器 | mmWave/PIR，人来开机、人走关投影 |
| 红外发射 | 控其他红外设备（H3S 本身是蓝牙，不需要） |
| Home Assistant | MQTT discovery，接 HA 做更复杂的自动化 |
| Matter | 如果米家支持作为 Matter 控制器，可试 OnOff 设备 |
| 小爱音箱 | 加一个便宜音箱，米家场景卡片方案就可用 |

---

## 12. 新会话交接提示

新开会话时，第一段可以直接复制：

```text
项目在 D:\demo\DSH\DSP32test\xgimi_mijia。
先读 README.md、PROJECT_STATE.md、CHANGELOG.md，再帮我加 <功能>。
约束：
- ESP32 core 3.3.13，分区 huge_app，上传 460800
- 不要动 BLE token / Wi-Fi / 巴法主题 touying001
- 状态检测 8080 优先，不要改回只轮询 554
- 米家不显示第三方卡片是已知限制，不要在这上面继续换 001/006
```

小修小改可以继续原会话；新功能建议新会话，文件就是上下文。
