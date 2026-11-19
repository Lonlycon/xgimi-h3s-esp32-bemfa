/*
 * 极米 H3S 米家开关（ESP32 + 巴法云）
 * ------------------------------------------------------------
 * on  -> BLE 广播唤醒投影（模拟原装蓝牙遥控器的开机包）
 * off -> 局域网 UDP 16750 快速关机
 * 状态 -> 轮询 TCP 端口，回传 on/off 给巴法云，米家开关不显示错
 *
 * 依赖：Arduino-ESP32 core + PubSubClient
 * 烧录：ESP32 Dev Module，默认分区，115200 串口
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_defs.h>
#include <esp_gap_ble_api.h>

// Arduino-ESP32 3.x 的 initArduino() 会检查 bleInUse()：
// 默认返回 false 时会释放 BLE 内存，之后 esp_bt_controller_init() 就会失败
// （我们实测返回 ESP_ERR_INVALID_STATE）。
// 这里提供强符号，告诉核心“本固件要用 BLE”，不要释放。
bool bleInUse(void) {
  return true;
}

// ===================== 配置 =====================
// 敏感配置在 secrets.h（已在 .gitignore 中忽略）。
// 首次使用：复制 secrets.example.h 为 secrets.h，填入自己的值。
#include "secrets.h"

// 板载 LED 引脚，-1 表示不用
#define STATUS_LED      2

// ============================================================

static const uint16_t UDP_KEY_PORT    = 16735;  // 简单按键
static const uint16_t UDP_CMD_PORT    = 16750;  // 复杂指令
static const uint32_t BLE_ADV_MS      = 10000;  // 开机广播持续时长
static const uint32_t BOOT_TIMEOUT_MS = 75000;  // 开机等待上限
static const uint32_t POLL_MS         = 8000;   // 状态轮询间隔
static const uint32_t MQTT_RETRY_MS   = 5000;
static const char*    MQTT_HOST       = "bemfa.com";
static const uint16_t MQTT_PORT       = 9501;

// 快速关机（16750 复杂指令）
static const char* CMD_FAST_OFF =
  "{\"action\":20000,\"controlCmd\":{\"delayTime\":0,\"mode\":6,\"time\":0,\"type\":0},\"msgid\":\"2\"}";

// 判断“投影是否开机”的 TCP 端口，依次尝试（H3S 实测 8080 开着，554 关闭）
static const uint16_t STATE_PORTS[] = { 8080, 554, 5555, 6466 };

enum State { ST_UNKNOWN, ST_OFF, ST_BOOTING, ST_ON, ST_STOPPING };
static State state = ST_UNKNOWN;

static IPAddress   projIp;
static WiFiUDP     udp;
static WiFiClient  netClient;
static PubSubClient mqtt(netClient);

static uint32_t bootAt        = 0;
static uint32_t pollAt        = 0;
static uint32_t mqttAt        = 0;
static uint32_t wifiAt        = 0;
static uint32_t offCmdAt      = 0;
static uint32_t pollMuteUntil = 0;

static uint8_t  bootRetry  = 0;
static uint8_t  bootUpHits = 0;
static uint8_t  offFailCnt = 0;
static bool     offRetried = false;
static bool     needPublish = false;

static volatile bool cmdOn  = false;
static volatile bool cmdOff = false;

// BLE
static uint8_t  advData[31];
static uint8_t  advLen = 0;
static uint8_t  scanRsp[31];
static uint8_t  scanRspLen = 0;
static volatile bool advReady = false;
static volatile bool rspReady = false;
static bool     bleReady      = false;
static bool     bleTokenOk    = false;
static bool     bleAdvertising = false;
static uint32_t bleStopAt     = 0;

// ===================== 小工具 =====================

static bool hexToBytes(const char* hex, uint8_t* out, size_t n) {
  if (!hex || strlen(hex) != n * 2) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < n; i++) {
    int hi = nib(hex[2 * i]);
    int lo = nib(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static void logState(const char* tag) {
  Serial.printf("[%s] state=%d\n", tag, (int)state);
}

// ===================== BLE 广播 =====================

static void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  if (event == ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT) {
    advReady = (param->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS);
  } else if (event == ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT) {
    rspReady = (param->scan_rsp_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS);
  }
}

static bool bleInit() {
  esp_err_t err;

  err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  Serial.printf("[BLE] mem_release=%s\n", esp_err_to_name(err));

  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  cfg.mode = ESP_BT_MODE_BLE;  // 经典蓝牙内存会被核心释放，这里只初始化 BLE
  err = esp_bt_controller_init(&cfg);
  Serial.printf("[BLE] controller_init=%s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;

  err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  Serial.printf("[BLE] controller_enable=%s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;

  err = esp_bluedroid_init();
  Serial.printf("[BLE] bluedroid_init=%s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;

  err = esp_bluedroid_enable();
  Serial.printf("[BLE] bluedroid_enable=%s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;

  esp_ble_gap_set_device_name("Bluetooth 4.0 RC");
  esp_ble_gap_register_callback(gapCallback);
  return true;
}

// 组装 31 字节广播包 + 18 字节扫描响应
static bool buildAdvPackets() {
  uint8_t* p = advData;
  // Flags
  *p++ = 0x02; *p++ = 0x01; *p++ = 0x06;
  // 16-bit UUID 0x1812 (HID)
  *p++ = 0x03; *p++ = 0x03; *p++ = 0x12; *p++ = 0x18;
  // Appearance 961
  *p++ = 0x03; *p++ = 0x19; *p++ = 0xC1; *p++ = 0x03;
  // Manufacturer data: 0x13, 0xFF, company 0x0046, 16 字节 token
  *p++ = 0x13; *p++ = 0xFF; *p++ = 0x46; *p++ = 0x00;
  if (!hexToBytes(BLE_TOKEN_HEX, p, 16)) {
    Serial.println("[BLE] token 必须是 32 个十六进制字符");
    return false;
  }
  p += 16;
  advLen = (uint8_t)(p - advData);   // 31

  const char* name = "Bluetooth 4.0 RC";
  size_t n = strlen(name);
  scanRsp[0] = (uint8_t)(n + 1);
  scanRsp[1] = 0x09;                 // Complete Local Name
  memcpy(scanRsp + 2, name, n);
  scanRspLen = (uint8_t)(n + 2);     // 18
  return true;
}

static bool bleStartWake() {
  esp_ble_gap_stop_advertising();

  advReady = false;
  rspReady = false;
  esp_ble_gap_config_adv_data_raw(advData, advLen);
  esp_ble_gap_config_scan_rsp_data_raw(scanRsp, scanRspLen);

  uint32_t t0 = millis();
  while ((!advReady || !rspReady) && millis() - t0 < 1500) {
    delay(10);
  }
  if (!advReady || !rspReady) {
    Serial.println("[BLE] 配置广播数据失败/超时");
    return false;
  }

  esp_ble_adv_params_t params = {};
  params.adv_int_min       = 0x20;   // 20ms
  params.adv_int_max       = 0x40;   // 40ms
  params.adv_type          = ADV_TYPE_IND;
  params.own_addr_type     = BLE_ADDR_TYPE_PUBLIC;
  params.channel_map       = ADV_CHNL_ALL;
  params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

  esp_err_t err = esp_ble_gap_start_advertising(&params);
  if (err != ESP_OK) {
    Serial.printf("[BLE] 启动广播失败: %d\n", (int)err);
    return false;
  }
  Serial.println("[BLE] 正在发送开机广播...");
  return true;
}

// ===================== 极米 UDP =====================

static void udpSend(const char* msg, uint16_t port, uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
    udp.beginPacket(projIp, port);
    udp.write((const uint8_t*)msg, strlen(msg));
    udp.endPacket();
    delay(80);
  }
}

static bool projectorUp() {
  for (uint16_t port : STATE_PORTS) {
    WiFiClient c;
    c.setTimeout(300);
    if (c.connect(projIp, port, 300)) {
      c.stop();
      return true;
    }
  }
  return false;
}

// ===================== 状态发布 =====================

static void publishState(bool on) {
  if (!mqtt.connected()) return;
  mqtt.publish(BEMFA_TOPIC, on ? "on" : "off", false);
  Serial.printf("[MQTT] publish %s\n", on ? "on" : "off");
}

// ===================== 开 / 关 =====================

static void doPowerOn() {
  if (state == ST_ON || state == ST_BOOTING) return;
  Serial.println("[CMD] power on");
  if (!bleTokenOk) {
    Serial.println("[CMD] BLE token 无效，无法开机");
    publishState(false);
    return;
  }
  if (!bleReady) {
    Serial.println("[CMD] BLE 未初始化，无法开机");
    publishState(false);
    return;
  }
  if (!bleStartWake()) {
    state = ST_OFF;
    publishState(false);
    return;
  }
  bleAdvertising = true;
  bleStopAt      = millis() + BLE_ADV_MS;
  state          = ST_BOOTING;
  bootAt         = millis();
  bootRetry      = 0;
  bootUpHits     = 0;
  pollAt         = 0;              // 立刻开始轮询
  pollMuteUntil  = 0;
}

static void doPowerOff() {
  if (state == ST_OFF || state == ST_STOPPING) return;
  Serial.println("[CMD] power off");
  if (bleAdvertising) {
    esp_ble_gap_stop_advertising();
    bleAdvertising = false;
  }
  udpSend(CMD_FAST_OFF, UDP_CMD_PORT, 3);
  state      = ST_STOPPING;
  offCmdAt   = millis();
  offRetried = false;
  publishState(false);             // 乐观更新，米家立刻变 off
}

// ===================== MQTT =====================

static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  msg.toLowerCase();
  Serial.printf("[MQTT] %s -> %s\n", topic, msg.c_str());
  if (msg == "on")       cmdOn  = true;
  else if (msg == "off") cmdOff = true;
}

static void mqttEnsure() {
  if (mqtt.connected()) return;
  if (millis() - mqttAt < MQTT_RETRY_MS) return;
  mqttAt = millis();

  Serial.print("[MQTT] connecting ... ");
  if (mqtt.connect(BEMFA_UID)) {
    Serial.println("ok");
    mqtt.subscribe(BEMFA_TOPIC);
    needPublish = true;
  } else {
    Serial.printf("failed rc=%d\n", mqtt.state());
  }
}

// ===================== 状态轮询 =====================

static void pollState() {
  if (millis() < pollMuteUntil) return;
  bool up = projectorUp();

  switch (state) {
    case ST_BOOTING:
      if (up) {
        if (++bootUpHits >= 2) {          // 连续两次，避免刚开端口误判
          state = ST_ON;
          bootRetry = 0;
          publishState(true);
          logState("boot ok");
        }
      } else {
        bootUpHits = 0;
        if (millis() - bootAt > BOOT_TIMEOUT_MS) {
          if (bootRetry < 1) {
            bootRetry++;
            Serial.println("[BLE] 首次唤醒超时，重试一次");
            if (bleStartWake()) {
              bleAdvertising = true;
              bleStopAt      = millis() + BLE_ADV_MS;
              bootAt         = millis();
            }
          } else {
            state = ST_OFF;
            publishState(false);
            logState("boot timeout");
          }
        }
      }
      break;

    case ST_ON:
      if (!up) {
        if (++offFailCnt >= 3) {
          state = ST_OFF;
          offFailCnt = 0;
          publishState(false);
          logState("lost");
        }
      } else {
        offFailCnt = 0;
      }
      break;

    case ST_STOPPING:
      if (!up) {
        state = ST_OFF;
        publishState(false);
        logState("off ok");
      } else if (!offRetried && millis() - offCmdAt > 25000) {
        offRetried = true;
        Serial.println("[CMD] 关机超时，补发一次快速关机");
        udpSend(CMD_FAST_OFF, UDP_CMD_PORT, 2);
      } else if (millis() - offCmdAt > 45000) {
        state = ST_ON;                    // 关不掉就如实回到 on
        publishState(true);
        logState("off failed");
      }
      break;

    case ST_OFF:
      if (up) {                           // 有人用遥控器手动开了
        state = ST_ON;
        publishState(true);
        logState("external on");
      }
      break;

    default: // ST_UNKNOWN
      state = up ? ST_ON : ST_OFF;
      publishState(up);
      logState("init");
      break;
  }
}

// ===================== setup / loop =====================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== XGIMI H3S Mi Home switch (ESP32 + bemfa) ===");

  if (!projIp.fromString(PROJECTOR_IP)) {
    Serial.println("[CFG] PROJECTOR_IP 格式错误");
  }

  bleTokenOk = buildAdvPackets();
  if (!bleTokenOk) Serial.println("[CFG] BLE token 无效，开机功能不可用");

  bleReady = bleInit();
  if (!bleReady) Serial.println("[CFG] BLE 初始化失败，开机功能不可用");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                  // 降低 UDP 延迟
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] ok, ip=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] timeout, 稍后自动重连");
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(30);
  mqtt.setBufferSize(256);

  udp.begin(0);
  if (STATUS_LED >= 0) pinMode(STATUS_LED, OUTPUT);

  logState("start");
}

void loop() {
  // 串口调试命令（115200）：o=开机 f=关机 w=只发广播 s=状态
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c == 'o' || c == 'O') {
      cmdOn = true;
    } else if (c == 'f' || c == 'F') {
      cmdOff = true;
    } else if (c == 'w' || c == 'W') {
      Serial.println("[TEST] 发送 10 秒开机广播");
      if (bleTokenOk && bleReady && bleStartWake()) {
        bleAdvertising = true;
        bleStopAt     = millis() + 10000;
      } else {
        Serial.println("[TEST] BLE 未就绪或 token 无效");
      }
    } else if (c == 's' || c == 'S') {
      Serial.printf("[STATE] state=%d ip=%s mqtt=%d ble=%d adv=%d\n",
                    (int)state, PROJECTOR_IP, mqtt.connected() ? 1 : 0,
                    bleReady ? 1 : 0, bleAdvertising ? 1 : 0);
    }
  }

  // Wi-Fi 重连
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiAt > 10000) {
      wifiAt = millis();
      Serial.println("[WiFi] reconnect");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }

  mqttEnsure();
  mqtt.loop();

  // 广播到时间就停
  if (bleAdvertising && millis() > bleStopAt) {
    esp_ble_gap_stop_advertising();
    bleAdvertising = false;
    Serial.println("[BLE] stop");
  }

  // 处理命令（不在 MQTT 回调里做阻塞动作）
  if (cmdOn)  { cmdOn  = false; doPowerOn();  }
  if (cmdOff) { cmdOff = false; doPowerOff(); }

  // 重连成功后补发一次状态
  if (needPublish && mqtt.connected()) {
    needPublish = false;
    if (state == ST_ON)        publishState(true);
    else if (state == ST_OFF)  publishState(false);
  }

  uint32_t interval = (state == ST_BOOTING) ? 2000 : POLL_MS;
  if (millis() - pollAt > interval) {
    pollAt = millis();
    pollState();
  }

  // 状态灯：常亮=已开机，快闪=开机中，慢闪=其他
  if (STATUS_LED >= 0) {
    static uint32_t ledAt = 0;
    static bool ledOn = false;
    if (state == ST_ON) {
      digitalWrite(STATUS_LED, HIGH);
    } else {
      uint32_t period = (state == ST_BOOTING) ? 100 : 1500;
      if (millis() - ledAt > period) {
        ledAt = millis();
        ledOn = !ledOn;
        digitalWrite(STATUS_LED, ledOn ? HIGH : LOW);
      }
    }
  }
}
