# 项目状态（PROJECT_STATE）

> 更新时间：2026-11-21
> 当前版本：**v2.0**（网页遥控 + TCP 按键通道 + 电脑显示模式，全部实测）
> 一句话：ESP32 冒充极米 App 接管 H3S——蓝牙开机、UDP 关机、TCP 13145 发按键，
> 再通过巴法云接入小爱，并内置网页遥控。

---

## 1. 目标与边界

**已完成**

- 开关机（蓝牙唤醒 / UDP 快速关机）
- 按键（方向、确定、返回、菜单、音量）——TCP 13145 私有协议
- 网页遥控（不占巴法云主题）
- 语音控制（巴法云 → 米家 → 小爱）
- 电脑显示模式切换（扩展 / 复制 / 仅投影 / 仅电脑）

**不做 / 未完成**

- 对焦、信号源：**找不到协议指令**（`code` 0–50 已排除）
- 米家原生"投影仪"品类卡片（需小米 IoT 开放平台，DIY 不现实）
- 网页鉴权（局域网内开放）

---

## 2. 当前环境

| 项 | 值 |
|---|---|
| 开发板 | ESP32-D0WD-V3（CH340），**COM4** |
| 板子 MAC | `78:42:1c:6a:01:64` |
| ESP32 IP | `192.168.0.21`（路由器 DHCP 静态绑定） |
| 投影 IP | `192.168.0.30` |
| 电脑 IP（跑接收程序） | `192.168.0.10` |
| 路由器网关 | `192.168.0.1` |
| Wi-Fi | 2.4GHz（凭证在 `secrets.h`） |
| 巴法云主题 | `touying001` + `kuozhan001` / `fuzhi001` / `jinzhu001` / `jinpc001` |
| Arduino-ESP32 | 3.3.13 |
| PubSubClient | 2.8 |
| arduino-cli | 1.6.0（`tools/arduino-cli/`） |
| Python | 3.7+（电脑端）；`pystray` + `pillow` |

> 投影系统版本（**升级前的基准，务必记牢**）：
> `GMUI 5.1.100.02` / `v3.19.288` / Android 9 / mtk958

---

## 3. 已实现功能

| 功能 | 状态 | 说明 |
|---|---|---|
| BLE 广播开机 | ✅ | token 广播 10 秒，最长等 75 秒 |
| UDP 快速关机 | ✅ | `16750`，`mode=6` |
| 状态检测 | ✅ | `8080/554/5555/6466`，8080 优先 |
| 巴法云 MQTT | ✅ | 5 个主题，每 60 秒自动重订阅 |
| 小爱语音开关机 | ✅ | 手机小爱可用 |
| 网页遥控 | ✅ | 端口 80，固定尺寸按钮 |
| TCP 13145 按键 | ✅ | 1 左 / 2 右 / 3 上 / 4 下 / 5 确定 / 6 返回 / 9 音量+ / 10 音量− / 12 菜单 |
| 电脑显示模式 | ✅ | UDP 54321 → DisplaySwitch |
| 电脑端托盘 | ✅ | pystray，绿/灰/红三态 |
| 对焦 / 信号源 | ❌ | 无码值 |
| OTA | ❌ | 未实现 |

---

## 4. 架构

```
   手机小爱/米家 ── 巴法云 MQTT ── ESP32
                                    │
   手机浏览器 ── HTTP :80 ──────────┤
                                    │
                                    ├─ BLE 广播 ──▶ 开机
                                    ├─ UDP 16750 ─▶ 关机
                                    ├─ TCP 13145 ─▶ 按键 / 查询
                                    ├─ TCP 8080 ──▶ 状态探测
                                    └─ UDP 54321 ─▶ 电脑（显示模式）
```

### 三条通道为什么分开

| 通道 | 为什么必须用它 |
|---|---|
| BLE | 投影关机后网络全断，只有蓝牙能唤醒 |
| UDP 16750 | 老接口，只实现了关机（`mode=6`），其它 mode 不生效 |
| TCP 13145 | 新版 App 的完整控制通道，按键只能走这里 |

> 实测结论：UDP 16750 发 `mode=7` 无任何反应；同样的指令走 TCP 13145 才有回复。
> 说明 UDP 端只保留了关机分支。

---

## 5. 关键参数（不要随意改）

| 参数 | 值 | 原因 |
|---|---|---|
| `BLE_ADV_MS` | 10000 | 5 秒开机成功率不够 |
| `BOOT_TIMEOUT_MS` | 75000 | H3S 启动慢 |
| `POLL_MS` | 8000 | 状态轮询间隔 |
| `STATE_PORTS` | `{8080,554,5555,6466}` | H3S 的 554 是关的，8080 必须最前 |
| `UDP_CMD_PORT` | 16750 | 关机 |
| `PROJ_TCP_PORT` | 13145 | 按键/查询 |
| `PC_PORT` | 54321 | 电脑接收 |
| `MQTT_HOST` | `bemfa.com:9501` | 巴法云 |
| 重订阅间隔 | 60000 | 自愈"主题后建" |
| `bleInUse()` | 返回 true | 防核心释放 BLE 内存 |
| `cfg.mode` | `ESP_BT_MODE_BLE` | 经典蓝牙内存已释放 |
| 分区 | `huge_app` | 固件 1.69MB，默认 1.3MB 装不下 |
| 上传速度 | 460800 | 921600 在 CH340 上到 96% 掉线 |

---

## 6. 构建与烧录

```powershell
$cli  = "D:\demo\DSH\DSP32test\tools\arduino-cli\arduino-cli.exe"
$sk   = "D:\demo\DSH\DSP32test\xgimi_mijia"
$fqbn = "esp32:esp32:esp32:PartitionScheme=huge_app"

& $cli compile --fqbn $fqbn $sk
& $cli upload -p COM4 --fqbn $fqbn --upload-property upload.speed=460800 $sk
```

当前体积：**1692313 字节（53%）**，RAM **63424 字节（19%）**。

---

## 7. 验证步骤

### 7.1 固件

| 测试 | 预期 |
|---|---|
| 串口 `s` | 打印状态、ESP32 IP、投影 IP、电脑 IP |
| 网页 `http://192.168.0.21/` | 页面正常，状态栏每 3 秒刷新 |
| 网页点"菜单" | 投影弹出菜单 |
| 网页点"音量＋" | 音量 +2 |
| 网页点"扩展" | 电脑（.10）切到扩展 |
| 小爱"打开投影仪" | 投影开机（约 20–40 秒） |

### 7.2 巴法云

```text
http://apis.bemfa.com/vb/api/v2/topicInfo?openID=<私钥>&type=1&topic=touying001
```

应返回 `online:true`、`deviceType:"outlet"`。

### 7.3 TCP 13145 自检

```powershell
python tools\xgimi_pad.py 192.168.0.30 --inst 12    # 菜单，应弹菜单
python tools\xgimi_pad.py 192.168.0.30 --send 7 3   # 取应用列表，回 30236
```

---

## 8. 已知问题与限制

1. **对焦 / 信号源无指令**
   `50036` 通道的 `code` 0–50 全部试过，没有对焦或信号源。
   可能不在该通道，或需要 `type` 变化（如 `long`），或根本只有手机 App 能做。

2. **米家不显示卡片**
   巴法属"其他平台设备"，001/006 都一样，属平台限制。

3. **巴法云单连接限制**
   一个私钥同时只能一个 MQTT 连接 → 电脑端必须经 ESP32 中转，不能直连。

4. **网页无鉴权**
   同一局域网内任何人可打开。家用可接受，介意可加 Basic Auth。

5. **首次按键慢半拍**
   要建 TCP + 握手 + 注册，约 280ms；之后复用连接约 105ms。

6. **升级风险**
   固件依赖厂商私有协议。已有 H3 用户升级后 UDP 指令失效的先例。
   **升级投影前先记录系统版本并备份本项目。**

7. **敏感信息**
   Wi-Fi 密码、巴法私钥、BLE token 在 `secrets.h`（已 gitignore）。
   备份到网盘/公开仓库前先脱敏。

---

## 9. 文件清单

| 文件 | 作用 |
|---|---|
| `xgimi_mijia.ino` | 固件主程序（约 1300 行，含网页） |
| `README.md` | 使用 / 部署 / 协议说明 |
| `PROJECT_STATE.md` | 本文件，交接用 |
| `CHANGELOG.md` | 版本变更 |
| `secrets.h` / `secrets.example.h` | 配置（真实值不提交） |
| `tools/display_switch.py` | 电脑端接收程序（托盘版） |
| `tools/start_display_switch.bat` | 电脑端启动器（自动挑 Python） |
| `tools/xgimi_pad.py` | 交互式指令测试器 |
| `tools/xgimi_tcp.py` | TCP 协议握手探测 |
| `tools/mqtt_probe.py` | 巴法云订阅诊断 |
| `tools/udp_test.py` | UDP 指令测试 |
| `tools/ble_scan.py` | 电脑蓝牙抓 BLE token |
| `tools/download_esp32_offline.ps1` | 下载 ESP32 离线包 |

---

## 10. 版本管理

```bash
git tag          # v1.0 固定第一版，不可覆盖
```

- `v1.0`：蓝牙开机 + UDP 关机 + 巴法云
- `v2.0`：网页遥控 + TCP 按键 + 电脑显示模式

**发布新版本时不要 `git tag -f v1.0`**，用新 tag（如 `v2.1`）。

---

## 11. 后续可做

| 功能 | 说明 | 难度 |
|---|---|---|
| 对焦 / 信号源 | 需继续逆向；或改用 ADB | 中高 |
| OTA | ArduinoOTA / HTTP OTA | 中 |
| 网页鉴权 | Basic Auth，几行代码 | 低 |
| Wi-Fi 配网 | WiFiManager / AP 配网 | 中 |
| Home Assistant | MQTT discovery | 中 |
| 按键长按 | `type` 试 `long` / `down` / `up` | 低 |

---

## 12. 新会话交接提示

```text
项目在 D:\demo\DSH\DSP32test\xgimi_mijia，当前 v2.0。
先读 README.md、PROJECT_STATE.md、CHANGELOG.md，再帮我加 <功能>。

硬约束：
- ESP32 core 3.3.13，分区 huge_app，上传 460800，COM4
- 别动 bleInUse() 和 cfg.mode = ESP_BT_MODE_BLE
- 状态检测 8080 优先，不要只轮询 554
- 巴法主题 touying001 等 5 个，名字不能改
- 按键走 TCP 13145 的 50036/commonCmd，不是老的 UDP 16735
- 米家不显示第三方卡片是已知限制，别再折腾 001/006
```
