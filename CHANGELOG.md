# CHANGELOG

本项目遵循“能跑就tag”的原则；v1.0 是第一个完整可用版本。

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
- 手机小爱：“打开/关闭插座”可控制。

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
- 没有小爱音箱；米家场景的“小爱音箱 → 自定义指令”方案不可用。
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
