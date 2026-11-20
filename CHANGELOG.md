# CHANGELOG

本项目遵循"能跑就 tag"的原则。v1.0 是第一个完整可用版本，v2.0 加入了网页遥控与按键控制。

---

## v2.0 - 2026-11-21

### Added

- **网页遥控**：ESP32 内置 WebServer（端口 80），手机浏览器直接遥控，不占巴法云主题。
  - 电源（开机/关机/电源键）、方向+确定+返回+菜单、音量、电脑显示模式
  - 状态栏每 3 秒刷新（投影状态 / MQTT / 蓝牙 / 电脑 / IP）
  - 固定尺寸按钮，不随窗口缩放
- **TCP 13145 按键通道**：逆向极米 App 私有协议并实测跑通。
  - 帧格式：`FF FF E0 4A` + 4 字节大端长度 + UTF-8 JSON
  - 流程：`10000` 握手 → `10001`；`10003` 注册 → `10004`；`50036 + commonCmd` 发按键
  - 长连接复用，首次约 280ms，之后约 105ms
- **实测按键码**：1 左 / 2 右 / 3 上 / 4 下 / 5 确定 / 6 返回 / 9 音量+ / 10 音量− / 12 菜单
- **电脑显示模式切换**：ESP32 经 UDP 54321 转发给电脑端程序，调 `DisplaySwitch.exe`。
- **电脑端程序** `tools/display_switch.py`
  - 托盘图标（绿=监听 / 灰=暂停 / 红=出错），右键菜单、双击开日志
  - 日志 1MB 自动轮转，断线不影响主功能
  - 没装 pystray 时自动退回无托盘模式
- **电脑 IP 可在网页修改**，写入 flash（NVS），换网络不用重刷固件。
- **MQTT 定时重订阅**：每 60 秒重新订阅全部主题，修复"主题在设备上线后才创建导致一直离线"。
- 工具：`tools/xgimi_pad.py`（交互式指令测试）、`tools/xgimi_tcp.py`（协议探测）、
  `tools/mqtt_probe.py`（巴法云订阅诊断）、`tools/start_display_switch.bat`（启动器）。

### Changed

- **按键协议**：从 UDP 16735 `KEYPRESSES:xx` 改为 TCP 13145 + `50036/commonCmd`。
  旧协议是老板 App 的接口，H3S 当前固件不认。
- **按键表**：废弃来自 Z5 社区帖的旧码表（未经验证，且方向键数值可疑），改用实测值。
- **显示模式顺序**：页面按钮改为 仅电脑 / 复制 / 扩展 / 仅投影。
- **音量键名**：`vol+` / `vol-` 改为 `volup` / `voldn`。
  原因：URL 查询串里的 `+` 会被解码成空格，导致 `n=vol+` 永远匹配不到（实测 404）。
- **电源键**：改为按当前状态判断（已开机则关，已关机则开），不再依赖未知的电源码。
- 固件体积 1.64MB → 1.69MB（53%），RAM 19%。

### Fixed

- **WebServer 在 begin() 前调用 handleClient() 导致空指针重启**
  加 `webStarted` 守卫。
- **Wi-Fi 断线重连后网页服务失效**
  检测"断→通"跳变，`stop()` + `begin()` 重启服务。
- **巴法云主题显示离线**
  原因是主题在 ESP32 订阅之后才创建；已加每 60 秒自动重订阅。
- **`/api/cmd?n=vol+` 返回 404**
  URL 中 `+` 被解析为空格；改按键名 + JS 加 `encodeURIComponent`。

### Verified

- 网页全部接口 200，按键实测有效（菜单、方向键）。
- 音量：连发 2 次 `volup`，投影音量 0 → 4（每按 +2）。
- 电脑显示模式：`DisplaySwitch.exe` 四个模式手动验证通过。
- 托盘程序：无托盘兜底、托盘模式均启动成功。

### Known issues

- **对焦 / 信号源**无码值：`code` 0–50 全部试过无对应功能，推测不在按键通道内。
- 米家不显示第三方平台设备卡片（平台限制，小爱语音可用）。
- 网页无鉴权，同一局域网可访问。
- 巴法云一个私钥同时只能有一个 MQTT 连接，电脑端必须经 ESP32 中转。

---

## v1.0 - 2026-11-19

### Added

- BLE 广播开机：31 字节 raw advertisement，`0x0046` + 16 字节 token。
- UDP 快速关机：`16750` 发送 `{"action":20000,...}`。
- 状态检测：`8080/554/5555/6466` 多端口轮询。
- 巴法云 MQTT：主题 `touying001`，状态回传 `on/off`。
- 串口调试命令：`w`（只发广播）、`o`（完整开机）、`f`（关机）、`s`（状态）。
- Wi-Fi / MQTT 断线重连、看门狗式状态轮询。
- 文档：`README.md`、`PROJECT_STATE.md`、`CHANGELOG.md`。
- 工具：`tools/ble_scan.py`、`tools/udp_test.py`、离线包下载脚本。

### Verified

- `w` → 投影开机。
- `o` → `[boot ok] state=3`。
- `f` → `[off ok] state=1`。
- 巴法云 `topicInfo`：`online=true`、`deviceType=outlet`、`msg=on/off`。
- 手机小爱："打开/关闭插座"可控制。

### Fixed

- **BLE 初始化失败（`ESP_ERR_INVALID_STATE`）**
  Arduino-ESP32 3.x 的 `initArduino()` 在 `bleInUse()` 默认 false 时释放 BLE 内存；
  增加强符号 `bool bleInUse(void) { return true; }`，并把 `cfg.mode` 设为 `ESP_BT_MODE_BLE`。
- **`esp_bt_dev_set_device_name` 编译错误**
  改用 `esp_ble_gap_set_device_name`。
- **固件超过默认分区**
  改用 `PartitionScheme=huge_app`。
- **CH340 上传 96% 掉线**
  上传速度从 921600 改为 460800。
- **H3S 554 端口关闭导致状态一直 OFF**
  `STATE_PORTS` 改为 8080 优先。
- **巴法主题不一致**
  `touyingyi006` / `touying006` / `touying001` 三次对齐，最终使用 `touying001`。
- **5 秒开机广播不够稳**
  `BLE_ADV_MS` 从 5000 改为 10000。

### Known issues

- 米家不显示第三方平台设备卡片（001/006 都一样）；小爱语音可用。
- 没有小爱音箱；米家场景的"小爱音箱 → 自定义指令"方案不可用。
- token 可能滚动，失效需重新抓。
- 仅自用，不要商品化。

---

## v0.3 - 2026-11-19

### Added

- 2.4GHz Wi-Fi 独立 SSID，ESP32 成功联网。
- 巴法云 MQTT 连接与主题订阅。
- `o/f/w/s` 串口调试命令。
- 8080 端口状态检测。
- 10 秒 BLE 广播。

### Changed

- 巴法主题从 `touying006` 改为 `touying001`（插座类型）。
- 投影 IP 固定为局域网静态地址。

### Fixed

- 多频合一（5GHz + WPA3）导致 ESP32 无法连接：
  分离 2.4G（WPA2）后解决。

---

## v0.2 - 2026-11-18

### Added

- 原始 BLE 广播包（31 字节）和扫描响应。
- `bleInit()` 错误码日志（`esp_err_to_name`）。
- 分区/上传参数调整。

### Fixed

- `esp_bt_dev_set_device_name` → `esp_ble_gap_set_device_name`。
- `bleInUse()` 覆盖，修复 BLE 内存被核心释放。
- BLE-only 模式初始化。
- `huge_app` 分区。
- 460800 上传速度。

---

## v0.1 - 2026-11-18

### Added

- 初始工程：ESP32 + 巴法云 + 极米 H3S。
- BLE token 抓取脚本 `tools/ble_scan.py`。
- UDP 测试脚本 `tools/udp_test.py`。
- 巴法 MQTT 接入和状态回传。
- README 初版。

### Notes

- 首次编译发现分区不足。
- 首次运行发现 BLE 初始化失败，进入 v0.2 排查。
