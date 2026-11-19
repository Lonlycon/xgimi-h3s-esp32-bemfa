# 极米 H3S 遥控器（ESP32 + 巴法云）

用一块 ESP32 模拟极米 H3S 的蓝牙遥控器开机包 + 局域网 UDP 关机指令，
再通过巴法云接入小爱，实现“打开/关闭投影仪”。

当前 v1.0 状态：

- **小爱语音可用**：手机小爱、小爱音箱都可以说“打开/关闭投影仪”；
- **米家可能不显示设备卡片**：巴法云属于“其他平台设备”，米家通常只把它作为小爱/场景可用设备，不生成普通卡片；
- 没有小爱音箱时，可以用手机小爱的“快捷指令”做桌面按钮；
- 详细状态见 [PROJECT_STATE.md](PROJECT_STATE.md)，变更见 [CHANGELOG.md](CHANGELOG.md)。

> 方向键/音量属于“完整遥控器”，不在本工程范围。

## 1. 硬件

| 件 | 说明 |
|---|---|
| ESP32 开发板 | 当前实测：ESP32-D0WD-V3，CH340 串口，COM3 |
| 5V 电源 | **必须独立常电**，不要插投影仪 USB（投影关机后 ESP32 也会断电，就无法唤醒了） |
| 杜邦线 | 本方案不需要接投影，仅供电 |

接线：只给 ESP32 插 USB 供电即可。板载 LED（GPIO2）用作状态灯：
常亮=已开机，快闪=开机中，慢闪=未开机/启动中。

板子 MAC 可在设备管理器/路由器里查看。

## 2. 软件依赖（当前验证版本）

- Arduino-ESP32 core **3.3.13**
- `PubSubClient` **2.8**（Nick O'Leary）
- `arduino-cli` **1.6.0**（可选，命令行编译/烧录）
- Python **3.12** + `bleak`（只用电脑抓 token 时需要）

Arduino IDE 里需要：

- 开发板：**ESP32 Dev Module**
- 分区：**Huge APP (3MB No OTA/1MB SPIFFS)**
- 上传速度：**460800**

## 3. 配置

复制 `secrets.example.h` 为 `secrets.h`，在 `secrets.h` 里修改这 6 个宏：

```cpp
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"
#define PROJECTOR_IP    "192.168.1.50"
#define BLE_TOKEN_HEX   "461703EFB22440FFFFFF3043524B544D"
#define BEMFA_UID       "YOUR_BEMFA_PRIVATE_KEY"
#define BEMFA_TOPIC     "touying001"
```

敏感值不写进文档，实际值在 `secrets.h`（已 gitignore）：

| 配置 | 说明 |
|---|---|
| `WIFI_SSID` | 2.4GHz Wi-Fi 名称 |
| `WIFI_PASS` | 见 `secrets.h` |
| `PROJECTOR_IP` | 投影仪 IP（如 `192.168.1.50`） |
| `BLE_TOKEN_HEX` | 见 `secrets.h` |
| `BEMFA_UID` | 见 `secrets.h` |
| `BEMFA_TOPIC` | `touying001`（001 = 插座） |

> `secrets.h` 里包含 Wi-Fi 密码、巴法私钥、BLE token，已在 `.gitignore` 中。备份到网盘/仓库前先脱敏，或者只保存在本地。

### 3.1 投影仪 IP

在路由器后台给 H3S 做 **DHCP 静态绑定**，否则 IP 变了固件就失控。
投影开机后，在 设置 → 网络 → 网络信息 里能看到当前 IP。

示例：路由器网关 `192.168.1.1`，投影 `192.168.1.50`，ESP32 由 DHCP 分配。

### 3.2 抓 BLE token（开机包）

**方法 A：手机（最直观）**

1. 投影仪**彻底断电**（拔插头，不是待机）。
2. 手机装 **nRF Connect**（安卓）或 **EFR Connect**。
3. 按遥控器电源键，扫描设备，找到名字 `Bluetooth 4.0 RC`。
4. 看 Manufacturer data，Company ID 是 `0x0046`；后面 16 字节就是 token。
   例：`46 00 | 46 17 03 EF B2 24 40 FF FF FF 30 43 52 4B 54 4D`
   → token = `461703EFB22440FFFFFF3043524B544D`
5. 填到 `BLE_TOKEN_HEX`（去掉空格，32 个字符）。

**方法 B：只用电脑（Windows 自带蓝牙）**

1. 投影拔电，手机不用参与。
2. 电脑装依赖：`pip install -U bleak`
3. 在 `xgimi_mijia` 目录运行：
   ```bash
   python tools/ble_scan.py
   ```
4. 对着投影连按遥控器电源键，脚本会打印 `0x0046` 对应的 16 字节 token。
5. 把 token 填到 `BLE_TOKEN_HEX`。

注意：

- Windows 普通应用只能**收** BLE 广播，不能稳定地**发**自定义广播；验证 token 用 ESP32 串口命令 `w`（见第 4 节）。
- **nRF Connect for Desktop 不行**，它只认 Nordic 自己的 dongle，不认电脑内置蓝牙。
- 电脑蓝牙必须是 BLE 4.0 及以上；很老的 2.1/3.0 适配器扫不到。
- 扫不到就再跑一次，或把遥控器贴近电脑；Windows 的蓝牙缓存偶尔会漏包。

> token 前几个字节可能随遥控器每次广播递增。多数人一直用同一个旧 token
> 也能开机；如果某天失效，重新抓一次即可。

### 3.3 巴法云

1. 打开 <https://cloud.bemfa.com> 注册并登录。
2. 控制台 → 主题 → 新建主题：`touying001`
   - 后三位 `001` 表示“插座”设备；
   - `006` 表示“开关”，也支持语音，但米家对这两种类型通常都不显示普通卡片。
3. 记下控制台里的**私钥**，填到 `BEMFA_UID`。

> 主题名必须和固件里的 `BEMFA_TOPIC` **完全一致**。
> 之前踩过 `touying006` / `touyingyi006` 不一致的坑：设备一直显示离线，米家同步不到。

## 4. 烧录

1. Arduino IDE 打开 `xgimi_mijia/xgimi_mijia.ino`。
2. 开发板选 **ESP32 Dev Module**，端口选对。
3. **分区方案选 `Huge APP (3MB No OTA/1MB SPIFFS)`**。
   默认分区只有 1.3MB，本固件约 1.64MB，会报 `Sketch too big`。
   arduino-cli 对应 FQBN：`esp32:esp32:esp32:PartitionScheme=huge_app`。
4. 上传，打开串口监视器 **115200**。

> **上传速度**：921600 在部分 CH340 板子上会到 96% 掉线，改用 **460800**。
> Arduino IDE：Upload Speed 选 460800。
> arduino-cli：加 `--upload-property upload.speed=460800`。

正常启动日志：

```text
=== XGIMI H3S Mi Home switch (ESP32 + bemfa) ===
[BLE] mem_release=ESP_OK
[BLE] controller_init=ESP_OK
[BLE] controller_enable=ESP_OK
[BLE] bluedroid_init=ESP_OK
[BLE] bluedroid_enable=ESP_OK
[WiFi] ok, ip=192.168.1.123
[MQTT] connecting ... ok
[init] state=1
```

`state=1` 是关机，`state=3` 是开机，`state=2` 是开机中。

### 4.1 串口调试命令（推荐先做）

烧录后，直接在串口监视器里发单个字符（不用加回车）：

| 命令 | 作用 |
|---|---|
| `w` | 只发 10 秒开机广播，不改状态机（**验证 token 用这个**） |
| `o` | 走完整开机流程：广播 → 等开机 → 回传 on |
| `f` | 快速关机 |
| `s` | 打印状态、IP、MQTT/BLE 状态 |

建议顺序：先发 `w` 看投影能不能开，再发 `f` 看能不能关，最后再接米家。

## 5. 小爱 / 米家接入

1. 米家 App → **我的 → 其他平台设备 → 添加 → 巴法**。
2. 输入巴法云账号密码，点“同步设备”。
3. 巴法云里会有一个叫 **“插座”** 的设备（`touying001`）。
4. 对小爱说：“打开插座” / “关闭插座”，或改名后说“打开投影仪”。

**重要：米家通常不会显示普通设备卡片。** 巴法属于“其他平台设备”，米家一般只把它作为小爱/场景可用设备。实际可用方式：

- 手机小爱语音（当前已验证可用）；
- 小爱音箱语音；
- 米家手动场景（如果执行动作里能选到“插座”）；
- 没有小爱音箱时：用手机小爱的“快捷指令”做桌面按钮。

米家设备列表里没有卡片，不代表同步失败；先用小爱语音确认能不能控制。

## 6. 先做协议验证（可选，但强烈建议）

烧录前，用电脑和投影在同一局域网，先确认 UDP 能关机。工程里带了脚本：

```bash
python tools/udp_test.py 192.168.1.50 off     # 快速关机
python tools/udp_test.py 192.168.1.50 power   # 电源键
python tools/udp_test.py 192.168.1.50 vol+    # 音量加
```

等价于手动发：

```python
import socket
ip = "192.168.1.50"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
# 快速关机
s.sendto(b'{"action":20000,"controlCmd":{"delayTime":0,"mode":6,"time":0,"type":0},"msgid":"2"}', (ip, 16750))
```

投影应直接进入关机/待机。如果不生效，先查 IP、防火墙、投影固件版本，
不要急着怀疑 ESP32。

## 7. 工作逻辑

```text
小爱/米家 "on"
   -> 巴法云 MQTT
   -> ESP32 发 10 秒 BLE 广播（token）
   -> H3S 开机
   -> ESP32 轮询 TCP 8080/554/5555/6466，确认开机后回传 "on"

小爱/米家 "off"
   -> ESP32 向 16750 发快速关机 JSON
   -> 轮询到端口关闭，回传 "off"

每 8 秒轮询一次；投影被遥控器手动打开/关闭，状态也会跟着变。
```

## 8. 关键实现 / 踩过的坑

1. **BLE 内存被 Arduino 核心释放**
   Arduino-ESP32 3.x 的 `initArduino()` 会检查 `bleInUse()`，默认 false 时释放 BLE 内存。之后 `esp_bt_controller_init()` 返回 `ESP_ERR_INVALID_STATE`，BLE 开机永远失败。
   修复：在 `.ino` 里提供强符号：
   ```cpp
   bool bleInUse(void) { return true; }
   ```
   并把控制器初始化改成 BLE-only：
   ```cpp
   cfg.mode = ESP_BT_MODE_BLE;
   ```
2. **H3S 的 554 端口是关的**
   状态检测改成 `STATE_PORTS = { 8080, 554, 5555, 6466 }`，8080 优先。
3. **开机广播 5 秒不够稳**
   `BLE_ADV_MS` 改成 **10000**。
4. **分区 / 上传速度**
   默认 1.3MB 分区装不下 1.64MB 固件，必须 `huge_app`；CH340 板子 921600 容易掉线，用 460800。
5. **巴法主题必须完全一致**
   代码、巴法控制台、米家同步三处的主题要一致；当前是 `touying001`。
6. **token 可能滚动**
   开机失败先重新抓 token；抓 token 用 `tools/ble_scan.py`。

## 9. 常见问题

| 现象 | 排查 |
|---|---|
| MQTT `rc=-2` | Wi-Fi 没连上 / DNS 失败，检查 2.4GHz Wi-Fi |
| MQTT `rc=5` | 私钥不对 / 未授权；检查 `BEMFA_UID` |
| 米家没有卡片 | 正常限制；先用小爱语音确认能控制；不要反复换 001/006 |
| 小爱没反应 | 设备名是否叫“插座/投影仪”；巴法是否在线；重启小爱/米家 |
| 开机没反应 | token 是否抓对；串口 `w` 看有没有发广播；ESP32 离投影是否太远 |
| 关机没反应 | UDP 16750 是否可达；先用 `tools/udp_test.py` 验证 |
| 状态相反 | 8080/554/5555/6466 都没开；改 `STATE_PORTS` 或投影设置里开 ADB |
| 上传到 96% 掉线 | 上传速度改 460800 |
| 频繁掉线 | 电源供电不足；换 5V/1A 以上电源；Wi-Fi 信号弱 |

## 10. 已知限制

- 米家通常不显示第三方平台设备卡片；小爱语音可用。
- 小爱 → 巴法云 → ESP32 有 1–3 秒云端延迟；开机本身还要 20–40 秒。
- 依赖巴法云第三方服务；如果巴法调整米家入口，需要换桥接方案。
- 极米固件升级可能关闭 UDP 端口或修改 BLE 包，届时需要重新抓包适配。
- token 可能随遥控器广播滚动，失效需重抓。
- 仅供自用，不要商品化；不要对接/仿冒小米云协议。

## 11. 备份 / 后续开发

- 当前可用版本建议打 tag：`v1.0`。
- 状态说明：[PROJECT_STATE.md](PROJECT_STATE.md)
- 变更记录：[CHANGELOG.md](CHANGELOG.md)
- 新功能建议另起新会话，先让新会话读 `PROJECT_STATE.md` 和本 README。
- 小修小改可以继续原会话；大功能（网页遥控、红外、传感器、Matter、HA）建议新会话。
