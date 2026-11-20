/*
 * ============================================================================
 *  极米 H3S 投影仪「开关 + 遥控」固件  ——  ESP32 + 巴法云 + 网页遥控
 *  v2.0（小白注释版）
 * ============================================================================
 *
 *  【v2.0 比 v1.0 多了什么】
 *    1. 网页遥控：手机浏览器打开 http://<ESP32的IP>/ 就能按方向键、音量、
 *       对焦、信号源、切电脑显示模式 —— 不占巴法主题、不加米家设备。
 *    2. 显示模式转发：米家场景/小爱 → 巴法云 → ESP32 → 局域网 UDP → 电脑
 *       上跑的小程序 → DisplaySwitch.exe 切扩展/复制/仅投影/仅电脑。
 *       为什么要绕这一圈？因为巴法云"一个私钥只能有一个 MQTT 连接"，
 *       电脑不能自己去连巴法云，否则会和 ESP32 互相顶下线。
 *    3. 按键码试打器：网页上可以手动输入任意按键码发给投影，
 *       用来探测"对焦""信号源"这些还没抓到码值的按键。
 *    4. 电脑 IP 可以在网页上改，并保存进 flash（NVS），换网络不用重烧固件。
 *
 *  【v1.0 原有功能（全部保留，逻辑未改）】
 *    - 蓝牙广播开机（token）
 *    - UDP 16750 快速关机
 *    - 巴法云 MQTT 主题 touying001 收发 on/off
 *    - TCP 端口轮询判断投影开/关机
 *    - 串口调试命令 o / f / w / s
 *    - 板载 LED 状态灯
 *
 *  【数据流全景】
 *
 *     手机小爱 / 米家场景
 *             │  巴法云 MQTT
 *             ▼
 *        ┌─────────────┐        蓝牙广播 31 字节 ────→ 投影开机
 *        │    ESP32    │─────── UDP 16750 ──────────→ 投影关机
 *        │             │─────── UDP 16735 ──────────→ 投影按键
 *        └─────────────┘
 *             │  ▲
 *   网页 HTTP │  │ UDP 54321（显示模式指令）
 *             ▼  ▼
 *      手机浏览器  电脑上的 display_switch.py ──→ DisplaySwitch.exe
 *
 *  【网页地址】
 *    看串口日志里 [WiFi] ok, ip=192.168.x.x 那一行。手机连同一个 Wi-Fi，
 *    浏览器打开 http://192.168.x.x/ 即可。
 *
 *  【改代码前的红线（和 v1 一样）】
 *    1. 别动 bleInUse() 和 cfg.mode = ESP_BT_MODE_BLE
 *    2. 别改 STATE_PORTS 的顺序（8080 必须最前）
 *    3. 别改 BEMFA_TOPIC 的值（要和巴法云控制台一致）
 *
 *  【备份】
 *    v1.0 原始版：xgimi_mijia.ino.v1.0.bak
 *    v1.0 注释版：xgimi_mijia.ino.v1.0-commented.bak
 *
 *  【依赖】
 *    Arduino-ESP32 核心 3.3.13 + PubSubClient 2.8
 *    （WebServer、Preferences 都是核心自带的，不用另外装库）
 *    编译（分区必须 huge_app）：
 *      arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" <本目录>
 * ============================================================================
 */


// ===========================================================================
//  第 1 部分：引入头文件
//  ---------------------------------------------------------------------------
//  #include 的作用：把别人写好的代码"复制"进来，这样我们才能用其中的功能。
// ===========================================================================

#include <WiFi.h>              // Wi-Fi 功能的工具箱：连接路由器、获取 IP
#include <WiFiUdp.h>           // UDP 发包功能的工具箱：给投影/电脑发指令
#include <PubSubClient.h>      // MQTT 客户端（第三方库）：和巴法云通信
#include <esp_bt.h>            // ESP32 底层蓝牙控制：初始化/释放蓝牙内存
#include <esp_bt_main.h>       // 蓝牙协议栈（Bluedroid）主控：init / enable
#include <esp_bt_defs.h>       // 蓝牙相关的常量与错误码定义
#include <esp_gap_ble_api.h>   // GAP 层接口：设置广播数据、启动广播
#include <WebServer.h>         // ★v2★ 网页服务器：手机浏览器遥控
#include <Preferences.h>       // ★v2★ 往 flash 里存小配置（电脑的 IP）


// ===========================================================================
//  第 2 部分：一个必须存在的"强符号"（本文件最容易删错的地方）
//  ---------------------------------------------------------------------------
//  Arduino-ESP32 3.x 启动时会问"有人要用蓝牙吗？"，判断依据就是这个函数。
//  官方默认返回 false → 核心把蓝牙内存还给系统 → 之后初始化蓝牙必然失败
//  （ESP_ERR_INVALID_STATE），开机功能彻底不能用。
//  所以我们自己重新定义它，固定返回 true，"骗"核心说"我要用蓝牙"。
// ===========================================================================

bool bleInUse(void) {   // 函数名、参数、返回值必须和官方声明完全一致才能覆盖
  return true;          // 永远回答"是，我要用蓝牙"
}


// ===========================================================================
//  第 3 部分：配置项
//  ---------------------------------------------------------------------------
//  敏感信息（Wi-Fi 密码、巴法云私钥、蓝牙 token）在 secrets.h 里（已 gitignore）。
// ===========================================================================

#include "secrets.h"      // 双引号表示"先找本目录下的文件"

// ★v2 新增★ 电脑的局域网 IP 和接收端口。
// 优先级：网页上保存的值（存在 flash）> secrets.h 里的 PC_IP > 下面这个默认值。
// 所以不改代码也能用：打开网页在最下面"设置 PC 地址"里填一次即可。
#ifndef PC_IP
#define PC_IP    "192.168.0.10"    // ← 默认值；secrets.h 里定义了就优先用那里的
#endif
#ifndef PC_PORT
#define PC_PORT 54321             // 电脑上 display_switch.py 监听的端口
#endif

// 板载状态灯接在哪个引脚。-1 表示"没有灯/不想用灯"
#define STATUS_LED      2

// ===========================================================================
//  第 4 部分：常量参数
//  ---------------------------------------------------------------------------
//  uint16_t = 16 位无符号整数（0~65535）  uint32_t = 32 位（0~42 亿）
//  static = 只在当前文件可见   const = 只读，运行时不能改
// ===========================================================================

// ---- v1 原有：极米 UDP 协议 ----
static const uint16_t UDP_KEY_PORT    = 16735;  // 简单按键协议：发 "KEYPRESSES:116"
static const uint16_t UDP_CMD_PORT    = 16750;  // 复杂指令协议：发 JSON（快速关机等）
static const uint32_t BLE_ADV_MS      = 10000;  // 开机广播持续 10 秒（5 秒不够稳）
static const uint32_t BOOT_TIMEOUT_MS = 75000;  // 开机最多等 75 秒
static const uint32_t POLL_MS         = 8000;   // 状态轮询间隔 8 秒
static const uint32_t MQTT_RETRY_MS   = 5000;   // MQTT 断线后每 5 秒重连一次
static const char*    MQTT_HOST       = "bemfa.com";
static const uint16_t MQTT_PORT       = 9501;

// 快速关机的 JSON（照抄抓包结果，不要手改）
static const char* CMD_FAST_OFF =
  "{\"action\":20000,\"controlCmd\":{\"delayTime\":0,\"mode\":6,\"time\":0,\"type\":0},\"msgid\":\"2\"}";

// 判断"投影是否开机"的 TCP 端口，8080 必须放最前（H3S 的 554 是关的）
static const uint16_t STATE_PORTS[] = { 8080, 554, 5555, 6466 };

// ---- ★v2 新增：网页与显示模式 ----
// 网页端口。80 = 浏览器直接输 IP 就能打开，不用带 :8080
static const uint16_t WEB_PORT = 80;

// 显示模式的 4 个巴法云主题（和米家同步的设备一一对应）
// 命名规则：前缀随便起，后缀数字是"设备类型"，001 = 插座
static const char* DISP_TOPICS[] = { "kuozhan001", "fuzhi001", "jinzhu001", "jinpc001" };
// 与上面一一对应的动作名，会原样通过 UDP 发给电脑上的小程序
static const char* DISP_CMDS[]   = { "EXTEND",      "CLONE",    "EXTERNAL",  "INTERNAL" };
static const uint8_t DISP_COUNT  = 4;

// ★v2.1：按键码 —— 全部在 H3S 上实测验证过
// 发送格式：{"action":50036,"commonCmd":{"action":"3","code":"<code>","type":"inst"}}
// 注意：这是"按键通道"，和 controlCmd(20000) 那条系统通道是两回事。
struct KeyEntry {
  const char* name;
  int16_t     code;
};

static const KeyEntry KEYS[] = {
  { "left",   1 },   // 左     ✅ 实测
  { "right",  2 },   // 右     ✅ 实测
  { "up",     3 },   // 上     ✅ 实测
  { "down",   4 },   // 下     ✅ 实测
  { "ok",     5 },   // 确定   ✅ 实测
  { "back",   6 },   // 返回   ✅ 实测
  { "volup",  9 },   // 音量加 ✅ 实测（名字不能带 +，URL 里 + 会被当成空格）
  { "voldn", 10 },   // 音量减 ✅ 实测
  { "menu",  12 },   // 菜单   ✅ 实测
};
static const uint8_t KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]);


// ===========================================================================
//  第 5 部分：状态机
//  ---------------------------------------------------------------------------
//  串口打印的 state= 数字就是下面这 5 个编号：
//    0 未知   1 已关机   2 开机中   3 已开机   4 关机中
// ===========================================================================

enum State {
  ST_UNKNOWN,          // 0：刚启动，还没探测过
  ST_OFF,              // 1：确认为关机
  ST_BOOTING,          // 2：已发开机广播，等投影启动
  ST_ON,               // 3：确认为开机
  ST_STOPPING          // 4：已发关机指令，等投影关闭
};

static State state = ST_UNKNOWN;


// ===========================================================================
//  第 6 部分：全局变量
// ===========================================================================

static IPAddress   projIp;      // 投影的 IP
static IPAddress   pcIp;        // ★v2★ 电脑的 IP（发显示模式指令用）
static WiFiUDP     udp;         // UDP 对象（关机、按键、显示指令都用它）
static WiFiClient  netClient;   // TCP 客户端：给 MQTT 当电话线，也用来探测端口
static PubSubClient mqtt(netClient);
static WebServer   http(WEB_PORT);   // ★v2★ 网页服务器
static Preferences prefs;            // ★v2★ flash 配置读写

// ---- 各种时间戳（都存 millis()，用来做非阻塞定时）----
static uint32_t bootAt        = 0;
static uint32_t pollAt        = 0;
static uint32_t mqttAt        = 0;
static uint32_t wifiAt        = 0;
static uint32_t offCmdAt      = 0;
static uint32_t pollMuteUntil = 0;   // 预留开关，当前恒为 0
static uint32_t resubAt       = 0;   // 上次"定时重新订阅"的时刻

// ---- 计数器 / 标志位 ----
static uint8_t  bootRetry  = 0;
static uint8_t  bootUpHits = 0;      // 连续 2 次探测到端口打开才算开机
static uint8_t  offFailCnt = 0;      // 连续 3 次探测失败才判定被关
static bool     offRetried = false;
static bool     needPublish = false;

// ---- 命令信箱：回调只留言，主循环再执行 ----
static volatile bool cmdOn  = false;
static volatile bool cmdOff = false;

// ★v2★ 显示模式留言：-1 表示没有；0~3 对应 DISP_CMDS 的下标
static volatile int8_t pendingDisp = -1;

// ★v2★ 网页服务器是否已启动过
static bool webStarted = false;
// ★v2★ Wi-Fi 上一轮是否在线（用来发现"断了又连上"，好重启网页服务）
static bool wifiWasUp = false;

// ---- 蓝牙 ----
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


// ===========================================================================
//  第 7 部分：小工具函数
// ===========================================================================

// 十六进制字符串 → 字节数组。例："461703" → out[0]=0x46, out[1]=0x17, out[2]=0x03
static bool hexToBytes(const char* hex, uint8_t* out, size_t n) {
  if (!hex || strlen(hex) != n * 2) return false;   // 每个字节要 2 个字符
  auto nib = [](char c) -> int {                    // 把字符转成 0~15 的数字
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < n; i++) {
    int hi = nib(hex[2 * i]);
    int lo = nib(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);             // 高 4 位 + 低 4 位拼成一个字节
  }
  return true;
}

// 打印一行状态日志：[boot ok] state=3
static void logState(const char* tag) {
  Serial.printf("[%s] state=%d\n", tag, (int)state);
}


// ===========================================================================
//  第 8 部分：蓝牙（BLE）—— 用于"开机"
//  ---------------------------------------------------------------------------
//  原理：原装遥控器按电源键时会发一段特定蓝牙广播，投影收到就开机。
//        我们只要用 ESP32 发一模一样的广播，就等于按下了电源键。
//        核心是那 16 字节 token。
// ===========================================================================

// 蓝牙事件回调：系统在"配置广播数据完成"等时刻自动调用
static void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  if (event == ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT) {
    advReady = (param->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS);
  } else if (event == ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT) {
    rspReady = (param->scan_rsp_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS);
  }
}

// 初始化蓝牙，返回 true 表示成功。6 步顺序不能乱。
static bool bleInit() {
  esp_err_t err;

  // 第 1 步：把"经典蓝牙"的内存还给系统（我们只用 BLE）
  err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  Serial.printf("[BLE] mem_release=%s\n", esp_err_to_name(err));

  // 第 2 步：配置并初始化控制器
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  cfg.mode = ESP_BT_MODE_BLE;   // ★关键★ 只能 BLE-only
  err = esp_bt_controller_init(&cfg);
  Serial.printf("[BLE] controller_init=%s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;

  // 第 3 步：启动控制器
  err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  Serial.printf("[BLE] controller_enable=%s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;

  // 第 4 步：初始化协议栈（Bluedroid）
  err = esp_bluedroid_init();
  Serial.printf("[BLE] bluedroid_init=%s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;

  // 第 5 步：启动协议栈
  err = esp_bluedroid_enable();
  Serial.printf("[BLE] bluedroid_enable=%s\n", esp_err_to_name(err));
  if (err != ESP_OK) return false;

  // 第 6 步：起个像原装遥控器的名字 + 登记回调
  esp_ble_gap_set_device_name("Bluetooth 4.0 RC");
  esp_ble_gap_register_callback(gapCallback);
  return true;
}

// 组装广播包（31 字节）+ 扫描响应包。格式是「长度 + 类型 + 数据」。
static bool buildAdvPackets() {
  uint8_t* p = advData;      // p 指向"当前要写入的位置"，写一个字节往后挪一格

  // 第 1 段：Flags（标志位）3 字节：02=长度，01=类型，06=支持 BLE 且可被发现
  *p++ = 0x02; *p++ = 0x01; *p++ = 0x06;

  // 第 2 段：16 位 UUID 0x1812（HID），表示"我是个遥控器"
  *p++ = 0x03; *p++ = 0x03; *p++ = 0x12; *p++ = 0x18;

  // 第 3 段：Appearance（外观）961 = 遥控器
  *p++ = 0x03; *p++ = 0x19; *p++ = 0xC1; *p++ = 0x03;

  // 第 4 段：制造商数据 20 字节 —— ★开机指令就藏在这里★
  // 13 = 后面 19 字节；FF = 类型；46 00 = 厂商号 0x0046（极米）
  *p++ = 0x13; *p++ = 0xFF; *p++ = 0x46; *p++ = 0x00;

  // 紧接着 16 字节 token
  if (!hexToBytes(BLE_TOKEN_HEX, p, 16)) {
    Serial.println("[BLE] token 必须是 32 个十六进制字符");
    return false;
  }
  p += 16;
  advLen = (uint8_t)(p - advData);   // 总长度 3+4+4+20 = 31，正好用满

  // 第 5 段：设备名放进"扫描响应包"（广播包已经满了）
  const char* name = "Bluetooth 4.0 RC";
  size_t n = strlen(name);
  scanRsp[0] = (uint8_t)(n + 1);
  scanRsp[1] = 0x09;                 // 类型"完整的设备名"
  memcpy(scanRsp + 2, name, n);
  scanRspLen = (uint8_t)(n + 2);     // 总长度 18
  return true;
}

// 开始发送开机广播
static bool bleStartWake() {
  esp_ble_gap_stop_advertising();    // 先停掉可能正在进行的广播

  advReady = false;
  rspReady = false;
  esp_ble_gap_config_adv_data_raw(advData, advLen);
  esp_ble_gap_config_scan_rsp_data_raw(scanRsp, scanRspLen);

  // 等回调通知"配置好了"，最多等 1.5 秒
  uint32_t t0 = millis();
  while ((!advReady || !rspReady) && millis() - t0 < 1500) {
    delay(10);
  }
  if (!advReady || !rspReady) {
    Serial.println("[BLE] 配置广播数据失败/超时");
    return false;
  }

  // 广播参数
  esp_ble_adv_params_t params = {};
  params.adv_int_min       = 0x20;   // 20ms
  params.adv_int_max       = 0x40;   // 40ms，喊得勤投影更容易听到
  params.adv_type          = ADV_TYPE_IND;                  // 可被连接的非定向广播
  params.own_addr_type     = BLE_ADDR_TYPE_PUBLIC;          // 用板子出厂 MAC
  params.channel_map       = ADV_CHNL_ALL;                  // 三个广播信道全用
  params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

  esp_err_t err = esp_ble_gap_start_advertising(&params);
  if (err != ESP_OK) {
    Serial.printf("[BLE] 启动广播失败: %d\n", (int)err);
    return false;
  }
  Serial.println("[BLE] 正在发送开机广播...");
  return true;
}


// ===========================================================================
//  第 9 部分：UDP 发送 —— 投影指令 + 电脑指令
//  ---------------------------------------------------------------------------
//  v1 只能发给投影，v2 拆成"发给任意 IP"的通用函数，
//  因为显示模式要发给电脑，按键要发给投影。
// ===========================================================================

// 通用版：把 msg 连发 times 次给 ip:port（多发几次对抗 UDP 丢包）
static void udpSendTo(IPAddress ip, uint16_t port, const char* msg, uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
    udp.beginPacket(ip, port);                        // 开始组包，目标是 ip:port
    udp.write((const uint8_t*)msg, strlen(msg));      // 写入内容（不含结尾的 \0）
    udp.endPacket();                                  // 收尾并真正发出去
    delay(80);                                        // 两次之间歇 80ms
  }
}

// v1 的旧函数保留原样：固定发给投影
static void udpSend(const char* msg, uint16_t port, uint8_t times) {
  udpSendTo(projIp, port, msg, times);
}

// 探测投影是否开机：挨个连 STATE_PORTS，能连上任何一个就算开机
static bool projectorUp() {
  for (uint16_t port : STATE_PORTS) {   // 8080 先试，最快返回
    WiFiClient c;                       // 临时客户端，用完即弃
    c.setTimeout(300);
    if (c.connect(projIp, port, 300)) { // 连上了说明投影系统在线
      c.stop();                         // 立刻断开（只是"敲门"）
      return true;
    }
  }
  return false;
}


// ===========================================================================
//  第 10 部分：把状态回报给云端
// ===========================================================================

static void publishState(bool on) {
  if (!mqtt.connected()) return;                        // 没连上就先不发
  mqtt.publish(BEMFA_TOPIC, on ? "on" : "off", false);  // false = 不保留消息
  Serial.printf("[MQTT] publish %s\n", on ? "on" : "off");
}


// ===========================================================================
//  第 11 部分：执行"开机"和"关机"两个动作
// ===========================================================================

static void doPowerOn() {
  if (state == ST_ON || state == ST_BOOTING) return;   // 已经开着，不用重复
  Serial.println("[CMD] power on");

  if (!bleTokenOk) {                    // 没 token，开不了
    Serial.println("[CMD] BLE token 无效，无法开机");
    publishState(false);
    return;
  }
  if (!bleReady) {                      // 蓝牙没起来，开不了
    Serial.println("[CMD] BLE 未初始化，无法开机");
    publishState(false);
    return;
  }
  if (!bleStartWake()) {                // 广播发不出去
    state = ST_OFF;
    publishState(false);
    return;
  }

  bleAdvertising = true;                  // 标记正在广播
  bleStopAt      = millis() + BLE_ADV_MS; // 10 秒后停
  state          = ST_BOOTING;            // 进入"开机中"
  bootAt         = millis();              // 记下开始时刻
  bootRetry      = 0;
  bootUpHits     = 0;
  pollAt         = 0;                     // 立刻开始轮询
  pollMuteUntil  = 0;
}

static void doPowerOff() {
  if (state == ST_OFF || state == ST_STOPPING) return;  // 已经关了
  Serial.println("[CMD] power off");

  if (bleAdvertising) {                 // 正在广播就停掉，防止刚关又被自己唤醒
    esp_ble_gap_stop_advertising();
    bleAdvertising = false;
  }

  udpSend(CMD_FAST_OFF, UDP_CMD_PORT, 3);   // 连发 3 次关机指令

  state      = ST_STOPPING;   // 进入"关机中"
  offCmdAt   = millis();
  offRetried = false;
  publishState(false);        // 乐观更新：先告诉云端"关了"，米家立刻变
}


// ===========================================================================
//  第 12 部分：MQTT —— 和巴法云通信
// ===========================================================================

// 收到云端消息时自动调用。必须"快"：只解析、只留言，不做耗时动作。
static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg;
  msg.reserve(len);                                   // 预分配，省内存
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();                                         // 去掉首尾空格/换行
  msg.toLowerCase();                                  // 统一小写，"ON" 也认
  Serial.printf("[MQTT] %s -> %s\n", topic, msg.c_str());

  // ★v2★ 先判断是不是"显示模式"的主题（kuozhan001 等）
  for (uint8_t i = 0; i < DISP_COUNT; i++) {
    if (strcmp(topic, DISP_TOPICS[i]) == 0) {
      if (msg == "on") pendingDisp = (int8_t)i;       // 留言，主循环再执行
      return;                                         // 不要落到下面的投影开关逻辑
    }
  }

  // v1 原有：投影开关
  if (msg == "on")       cmdOn  = true;
  else if (msg == "off") cmdOff = true;
}

// 订阅所有主题。verbose=true 时把每个主题的结果打到串口。
// 单独包一个函数的原因：除了"连上时订阅"，还要"定时重订阅"，
// 用来修复"主题是在设备上线之后才创建"或"某次订阅被服务器丢掉"的情况。
static bool subscribeAll(bool verbose) {
  bool allOk = true;

  bool ok = mqtt.subscribe(BEMFA_TOPIC);              // 投影开关主题
  if (verbose || !ok) Serial.printf("[MQTT] sub %-12s %s\n", BEMFA_TOPIC, ok ? "OK" : "FAIL");
  allOk &= ok;
  delay(30);                                          // 包与包之间留一点空隙

  for (uint8_t i = 0; i < DISP_COUNT; i++) {          // 4 个显示模式主题
    ok = mqtt.subscribe(DISP_TOPICS[i]);
    if (verbose || !ok) Serial.printf("[MQTT] sub %-12s %s\n", DISP_TOPICS[i], ok ? "OK" : "FAIL");
    allOk &= ok;
    delay(30);
  }
  return allOk;
}

// 确保 MQTT 连着，断了就重连（内部 5 秒节流）
static void mqttEnsure() {
  if (mqtt.connected()) return;
  if (millis() - mqttAt < MQTT_RETRY_MS) return;   // 减法写法能正确处理 millis 溢出
  mqttAt = millis();

  Serial.print("[MQTT] connecting ... ");
  if (mqtt.connect(BEMFA_UID)) {                   // 用巴法云私钥当客户端 ID
    Serial.println("ok");
    subscribeAll(true);                            // 订阅全部主题，并把结果打出来
    needPublish = true;                            // 稍后补报一次状态
  } else {
    Serial.printf("failed rc=%d\n", mqtt.state()); // -2 网络问题，5 私钥/授权问题
  }
}


// ===========================================================================
//  第 13 部分：状态轮询（状态机的引擎）
//  ---------------------------------------------------------------------------
//  开机中每 2 秒调一次，其它每 8 秒一次。
// ===========================================================================

static void pollState() {
  if (millis() < pollMuteUntil) return;
  bool up = projectorUp();

  switch (state) {

    // ---------- 正在开机，等它起来 ----------
    case ST_BOOTING:
      if (up) {
        if (++bootUpHits >= 2) {          // 连续 2 次都开才算（启动中端口会闪）
          state = ST_ON;
          bootRetry = 0;
          publishState(true);
          logState("boot ok");
        }
      } else {
        bootUpHits = 0;
        if (millis() - bootAt > BOOT_TIMEOUT_MS) {   // 超过 75 秒
          if (bootRetry < 1) {                       // 只重试一次
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

    // ---------- 已开机，盯着别被关掉 ----------
    case ST_ON:
      if (!up) {
        if (++offFailCnt >= 3) {          // 连续 3 次失败才认定被关（防网络抖动）
          state = ST_OFF;
          offFailCnt = 0;
          publishState(false);
          logState("lost");
        }
      } else {
        offFailCnt = 0;
      }
      break;

    // ---------- 已发关机指令，等它真的关掉 ----------
    case ST_STOPPING:
      if (!up) {
        state = ST_OFF;
        publishState(false);
        logState("off ok");
      } else if (!offRetried && millis() - offCmdAt > 25000) {  // 25 秒还没关，补发
        offRetried = true;
        Serial.println("[CMD] 关机超时，补发一次快速关机");
        udpSend(CMD_FAST_OFF, UDP_CMD_PORT, 2);
      } else if (millis() - offCmdAt > 45000) {                 // 45 秒还关不掉，认了
        state = ST_ON;
        publishState(true);
        logState("off failed");
      }
      break;

    // ---------- 已关机，盯着会不会被手动打开 ----------
    case ST_OFF:
      if (up) {
        state = ST_ON;
        publishState(true);
        logState("external on");
      }
      break;

    // ---------- 刚上电，第一次探测 ----------
    default: // ST_UNKNOWN
      state = up ? ST_ON : ST_OFF;
      publishState(up);
      logState("init");
      break;
  }
}


// ===========================================================================
//  第 14 部分：★v2 新增★ 显示模式转发（米家/小爱 → 电脑）
//  ---------------------------------------------------------------------------
//  为什么绕一圈？
//    巴法云用"私钥"当 MQTT 客户端 ID，一个私钥同时只能有一个连接。
//    如果电脑也去连巴法云，就会和 ESP32 互相顶下线。
//    所以让 ESP32 当中间人：收到巴法云命令后，用局域网 UDP 转发给电脑。
//
//  为什么收到后要立刻 publish "off"？
//    米家场景的动作是"打开插座"。第一次是 off→on，云端下发 on。
//    第二次点的时候设备已经是 on 了，云端可能认为"没变化"就不下发。
//    所以处理完立刻复位成 off，让它永远是个可反复触发的"按钮"。
// ===========================================================================

// 把显示模式指令发给电脑，返回是否发得出去
static bool sendDisplay(uint8_t idx) {
  if (idx >= DISP_COUNT) return false;
  if (pcIp == IPAddress(0, 0, 0, 0)) {          // 没配置有效 IP
    Serial.println("[DISP] PC IP 未配置，跳过");
    return false;
  }
  Serial.printf("[DISP] %s -> PC %s:%u\n", DISP_CMDS[idx], pcIp.toString().c_str(), PC_PORT);
  udpSendTo(pcIp, PC_PORT, DISP_CMDS[idx], 2);  // 发 2 次，电脑端会去重
  return true;
}

// 处理"待发送的显示模式指令"（主循环里调用，避免在 MQTT 回调里做阻塞动作）
static void handleDispPending() {
  int8_t idx = pendingDisp;
  if (idx < 0) return;
  pendingDisp = -1;
  if (idx >= (int8_t)DISP_COUNT) return;

  bool ok = sendDisplay((uint8_t)idx);
  // 无论成功与否都复位，否则米家那边会一直卡在"开"
  if (mqtt.connected()) mqtt.publish(DISP_TOPICS[idx], "off", false);
  Serial.printf("[DISP] %s\n", ok ? "已发送" : "发送失败（检查 PC IP）");
}


// ===========================================================================
//  第 15 部分：★v2 新增★ 网页遥控
//  ---------------------------------------------------------------------------
//  ESP32 自己起一个网站，手机浏览器连同一个 Wi-Fi 就能打开。
//  好处：不占巴法主题、不加米家设备、想加几个按钮就加几个。
//
//  ★v2.1 改动★
//    按键不再走老 UDP 16735，改成 TCP 13145 + 50036 通道（见 15.1）。
//    页面同时调整：返回/菜单并入"方向/确认"栏，删掉"功能键"栏，
//    所有按钮统一高度，音量单独一栏。
// ===========================================================================

// ---- 15.1 发按键给投影（TCP 13145 + 50036 通道）----
//
// 协议（从极米 App 抓包还原，并在 H3S 上逐条实测）：
//
//     FF FF E0 4A | 4字节大端长度 | UTF-8 JSON
//
//   连上 13145 之后必须先握手，否则发的指令会被当成"游客"：
//     10000 握手 → 10001（设备信息）
//     10003 注册 → 10004
//     之后：50036 + commonCmd  = 按键
//           20000 + controlCmd = 系统指令（关机等，另走 UDP 16750）
//
//   ★为什么不走老的 UDP 16735 "KEYPRESSES:xx"？
//     那套是老板 App 的协议，H3S 当前固件根本不认（实测无效）。
//     现在这套是官方 App 正在用的，码值也都是实测出来的。

static const uint16_t PROJ_TCP_PORT = 13145;

static WiFiClient projTcp;          // 和投影的长连接
static uint32_t   projTcpAt = 0;    // 上次尝试连接的时刻（做节流）

// 发一帧：魔数 + 4字节大端长度 + JSON
static bool pjSendFrame(const String& json) {
  uint32_t len = json.length();
  uint8_t hdr[8] = { 0xFF, 0xFF, 0xE0, 0x4A,
                     (uint8_t)(len >> 24), (uint8_t)(len >> 16),
                     (uint8_t)(len >> 8),  (uint8_t)(len) };
  if (projTcp.write(hdr, 8) != 8) return false;
  if (projTcp.write((const uint8_t*)json.c_str(), len) != len) return false;
  return true;
}

// 精确读 n 个字节，超时返回 false
static bool pjReadN(uint8_t* buf, size_t n, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < n) {
    if (!projTcp.connected()) return false;
    if (projTcp.available() > 0) {
      int r = projTcp.read(buf + got, (int)(n - got));
      if (r > 0) { got += (size_t)r; continue; }
    }
    if (millis() - t0 > timeoutMs) return false;
    delay(2);
  }
  return true;
}

// 读一帧，结果放进 out
static bool pjReadFrame(String& out, uint32_t timeoutMs) {
  uint8_t hdr[8];
  if (!pjReadN(hdr, 8, timeoutMs)) return false;
  if (hdr[0] != 0xFF || hdr[1] != 0xFF || hdr[2] != 0xE0 || hdr[3] != 0x4A) return false;
  uint32_t len = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) |
                 ((uint32_t)hdr[6] << 8)  |  (uint32_t)hdr[7];
  if (len == 0 || len > 4096) return false;
  char* buf = (char*)malloc(len + 1);
  if (!buf) return false;
  bool ok = pjReadN((uint8_t*)buf, len, timeoutMs);
  if (ok) {
    buf[len] = 0;
    out = String(buf);          // 只要 action 号，不做完整解析
  }
  free(buf);
  return ok;
}

// 确保控制通道已连接并完成握手 + 注册
static bool pjEnsure() {
  if (projTcp.connected()) return true;

  projTcp.stop();
  if (millis() - projTcpAt < 1000) return false;   // 1 秒内不重复尝试
  projTcpAt = millis();

  if (!projTcp.connect(projIp, PROJ_TCP_PORT, 600)) return false;

  // 10000 握手
  String hs = "{\"action\":10000,\"appid\":\"1\",\"authCode\":{\"authState\":-1,\"ip\":\"" +
              projIp.toString() +
              "\",\"phoneName\":\"ESP32\",\"phoneUid\":\"esp32-remote-01\",\"timeOut\":15000},\"msgid\":\"1\"}";
  if (!pjSendFrame(hs)) { projTcp.stop(); return false; }

  String resp;
  if (!pjReadFrame(resp, 900) || resp.indexOf("\"action\":10001") < 0) {
    projTcp.stop();
    return false;
  }

  // 10003 注册（拿不到 10004 也不影响发指令）
  String reg = "{\"action\":10003,\"endScreenCapTime\":0,\"msgid\":\"2\",\"startScreenCapTime\":0,"
               "\"type\":0,\"agreement\":{\"agreementType\":1,\"isTcp\":true,\"msgId\":\"2\","
               "\"phoneIp\":\"/" + WiFi.localIP().toString() + ":" +
               String(projTcp.localPort()) + "\",\"port\":13145}}";
  if (pjSendFrame(reg)) {
    String ack;
    pjReadFrame(ack, 500);
  }
  return true;
}

// 发一个按键码，返回是否成功送出去
static bool pjSendKey(int16_t code) {
  if (code < 0) return false;
  for (uint8_t attempt = 0; attempt < 2; attempt++) {
    if (!pjEnsure()) continue;
    String cmd = "{\"action\":50036,\"commonCmd\":{\"action\":\"3\",\"code\":\"" +
                 String((int)code) + "\",\"type\":\"inst\"}}";
    if (pjSendFrame(cmd)) {
      String echo;                 // 投影会回一条 50036 回声，顺手读掉
      pjReadFrame(echo, 120);
      return true;
    }
    projTcp.stop();
  }
  return false;
}

// 主循环里调用：把投影主动推来的帧读掉，避免缓冲越积越多
static void pjPoll() {
  if (!projTcp.connected()) return;
  while (projTcp.available() >= 8) {
    String tmp;
    if (!pjReadFrame(tmp, 20)) break;
  }
}

// 发"复杂指令"（16750 端口）。调焦等高级功能走这个协议。
static bool sendComplex(uint16_t mode, uint16_t type, uint16_t timev, uint16_t delayv) {
  char buf[128];
  int n = snprintf(buf, sizeof(buf),
                   "{\"action\":20000,\"controlCmd\":{\"delayTime\":%u,\"mode\":%u,\"time\":%u,\"type\":%u},\"msgid\":\"2\"}",
                   (unsigned)delayv, (unsigned)mode, (unsigned)timev, (unsigned)type);
  if (n <= 0 || n >= (int)sizeof(buf)) return false;
  udpSendTo(projIp, UDP_CMD_PORT, buf, 2);
  return true;
}

// ---- 15.2 网页内容（存在 flash 里，不占 RAM）----

static const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="theme-color" content="#14171c">
<title>极米 H3S 遥控</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;padding:12px;background:#14171c;color:#e8eaed;font:16px/1.5 -apple-system,"Segoe UI",system-ui,sans-serif}
h1{font-size:18px;margin:0 0 2px}
.sub{font-size:12px;color:#8b93a1;margin-bottom:10px}
.bar{font-size:12px;color:#8b93a1;margin-bottom:12px;line-height:1.9}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin:0 4px 0 10px;background:#555;vertical-align:middle}
.on{background:#3ecf8e}.off{background:#e05555}
section{background:#1c2027;border-radius:14px;padding:12px;margin-bottom:12px}
h2{font-size:12px;color:#8b93a1;margin:0 0 10px;font-weight:600;letter-spacing:1px}
.grid{display:grid;gap:8px;justify-content:center}
.g2{grid-template-columns:repeat(2,140px)}
.g3{grid-template-columns:repeat(3,104px)}
.g4{grid-template-columns:repeat(4,78px)}
button{border:0;border-radius:12px;background:#2a3038;color:#e8eaed;font-size:15px;height:56px;width:100%;padding:0 6px;cursor:pointer;transition:.08s}
button:active{background:#3a424e;transform:scale(.96)}
button.primary{background:#2b6cb0}
button.danger{background:#8f2f2f}
button.ghost{background:transparent}
input{width:100%;padding:12px;border-radius:10px;border:1px solid #333a45;background:#0f1216;color:#e8eaed;font-size:16px}
.row{display:flex;gap:8px;margin-top:8px}
.row input{flex:1}
.row button{width:auto;white-space:nowrap;padding-left:18px;padding-right:18px}
.warn{font-size:12px;color:#d69a45;margin-top:8px}
#toast{position:fixed;left:50%;bottom:22px;transform:translate(-50%,20px);background:#2a3038;border:1px solid #3a424e;padding:10px 18px;border-radius:20px;font-size:14px;opacity:0;transition:.18s;pointer-events:none;max-width:86vw;text-align:center}
#toast.show{opacity:1;transform:translate(-50%,0)}
</style></head><body>

<h1>极米 H3S 遥控</h1>
<div class="sub">ESP32 网页遥控 · 局域网直连</div>
<div class="bar" id="bar">正在读取状态…</div>

<section><h2>电源</h2><div class="grid g3">
<button class="primary" data-p="1">开机</button>
<button class="danger" data-p="0">关机</button>
<button data-t="1">电源键</button>
</div></section>

<section><h2>方向 / 确认</h2><div class="grid g3">
<button data-c="back">返回</button><button data-c="up">↑</button><button data-c="menu">菜单</button>
<button data-c="left">←</button><button data-c="ok">确定</button><button data-c="right">→</button>
<button class="ghost"></button><button data-c="down">↓</button><button class="ghost"></button>
</div></section>

<section><h2>音量</h2><div class="grid g2">
<button data-c="voldn">音量 −</button>
<button data-c="volup">音量 ＋</button>
</div></section>

<section><h2>电脑显示模式</h2><div class="grid g4">
<button data-d="3">仅电脑</button>
<button data-d="1">复制</button>
<button data-d="0">扩展</button>
<button data-d="2">仅投影</button>
</div>
<div class="warn">需要电脑上先运行 display_switch.py。不经过米家，也不占用巴法云。</div>
</section>

<section><h2>试码（找未知按键用）</h2>
<div class="row"><input id="code" type="number" inputmode="numeric" placeholder="输入按键码，如 12"><button id="sendKey">发送</button></div>
<div class="warn">走 50036 按键通道。已知：1左 2右 3上 4下 5确定 6返回 9音量+ 10音量- 12菜单。</div>
</section>

<section><h2>高级：复杂指令（16750）</h2>
<div class="row"><input id="mode" type="number" inputmode="numeric" placeholder="mode 值，如 6"><button id="sendCc">发送</button></div>
<div class="warn">未知 mode 可能有意外效果（已知 6 = 快速关机）。不确定就先别试。</div>
</section>

<section><h2>设置电脑地址</h2>
<div class="row"><input id="pcip" type="text" placeholder="192.168.1.100"><button id="savePc">保存</button></div>
<div class="warn">保存后写进 flash，断电不丢。填电脑的局域网 IP。</div>
</section>

<div id="toast"></div>
<script>
const $=s=>document.querySelector(s);
let tt;
function toast(t){const e=$('#toast');e.textContent=t;e.classList.add('show');clearTimeout(tt);tt=setTimeout(()=>e.classList.remove('show'),1800);}
async function hit(u){try{const r=await fetch(u);const t=await r.text();toast(t||'已发送');}catch(e){toast('网络错误');}}
document.querySelectorAll('[data-c]').forEach(b=>b.onclick=()=>hit('/api/cmd?n='+encodeURIComponent(b.dataset.c)));
document.querySelectorAll('[data-p]').forEach(b=>b.onclick=()=>hit('/api/power?on='+b.dataset.p));
document.querySelectorAll('[data-t]').forEach(b=>b.onclick=()=>hit('/api/power?toggle=1'));
document.querySelectorAll('[data-d]').forEach(b=>b.onclick=()=>hit('/api/disp?m='+b.dataset.d));
$('#sendKey').onclick=()=>{const v=$('#code').value.trim();if(!v)return toast('先输入按键码');hit('/api/key?c='+encodeURIComponent(v));};
$('#sendCc').onclick=()=>{const v=$('#mode').value.trim();if(!v)return toast('先输入 mode');hit('/api/cc?m='+encodeURIComponent(v));};
$('#savePc').onclick=()=>{const v=$('#pcip').value.trim();if(!v)return toast('先输入 IP');hit('/api/setpc?ip='+encodeURIComponent(v));};
const NAMES=['未知','已关机','开机中','已开机','关机中'];
async function tick(){
  try{
    const s=await (await fetch('/api/status')).json();
    const d=v=>'<span class="dot '+(v?'on':'off')+'"></span>';
    $('#bar').innerHTML='投影：<b>'+(NAMES[s.state]||s.state)+'</b>'
      +d(s.mqtt)+'MQTT'+d(s.ble)+'蓝牙'+d(s.pcok)+'电脑'
      +'<br>ESP32 '+s.ip+'　电脑 '+s.pcip;
    if(!$('#pcip').value) $('#pcip').value=s.pcip;
  }catch(e){$('#bar').textContent='与 ESP32 断开，检查 Wi-Fi';}
}
setInterval(tick,3000);tick();
</script>
</body></html>)HTML";

// ---- 15.3 网页接口处理 ----

// 首页：返回上面那一大段 HTML
static void handleRoot() {
  http.send_P(200, "text/html", PAGE_HTML);   // send_P 直接从 flash 发，不占 RAM
}

// GET /api/status → JSON 状态，页面每 3 秒拉一次
static void handleStatus() {
  String ip = WiFi.localIP().toString();
  String pc = pcIp.toString();
  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"state\":%d,\"ip\":\"%s\",\"pcip\":\"%s\",\"mqtt\":%d,\"ble\":%d,\"adv\":%d,\"pcok\":%d,\"up\":%lu}",
           (int)state, ip.c_str(), pc.c_str(),
           mqtt.connected() ? 1 : 0, bleReady ? 1 : 0, bleAdvertising ? 1 : 0,
           (pcIp == IPAddress(0, 0, 0, 0)) ? 0 : 1,
           (unsigned long)(millis() / 1000));
  http.send(200, "application/json", buf);
}

// GET /api/key?c=12 → 按码值发按键（走 50036 通道，可用于试码）
static void handleApiKey() {
  String s = http.arg("c");
  if (s.length() == 0) {
    http.send(400, "text/plain; charset=utf-8", "缺少按键码");
    return;
  }
  int code = s.toInt();
  if (code < 0 || code > 255) {
    http.send(400, "text/plain; charset=utf-8", "按键码无效（0~255）");
    return;
  }
  if (pjSendKey((int16_t)code)) {
    char out[48];
    snprintf(out, sizeof(out), "已发送 code=%d", code);
    http.send(200, "text/plain; charset=utf-8", out);
  } else {
    http.send(500, "text/plain; charset=utf-8", "发送失败（投影没连上？）");
  }
}

// GET /api/cmd?n=up → 按名字发按键
static void handleApiCmd() {
  String n = http.arg("n");
  for (uint8_t i = 0; i < KEY_COUNT; i++) {
    if (n == KEYS[i].name) {
      if (KEYS[i].code < 0) {
        http.send(200, "text/plain; charset=utf-8", String(KEYS[i].name) + " 还没有码值，请用试码找");
        return;
      }
      bool ok = pjSendKey(KEYS[i].code);
      http.send(ok ? 200 : 500, "text/plain; charset=utf-8",
                ok ? (String("已发送 ") + KEYS[i].name) : String("发送失败（投影没连上？）"));
      return;
    }
  }
  http.send(404, "text/plain; charset=utf-8", "没有这个按键");
}

// GET /api/power?on=1 / ?on=0 / ?toggle=1
// toggle=1 表示"电源键"：根据当前状态自动决定开还是关
static void handleApiPower() {
  if (http.arg("toggle") == "1") {
    if (state == ST_ON || state == ST_BOOTING) {
      cmdOff = true;
      http.send(200, "text/plain; charset=utf-8", "当前是开机，已排队关机");
    } else {
      cmdOn = true;
      http.send(200, "text/plain; charset=utf-8", "当前是关机，已排队开机");
    }
    return;
  }
  if (http.arg("on") == "1") {
    cmdOn = true;      // 留言，主循环执行（和 MQTT 一样）
    http.send(200, "text/plain; charset=utf-8", "开机指令已排队");
  } else {
    cmdOff = true;
    http.send(200, "text/plain; charset=utf-8", "关机指令已排队");
  }
}

// GET /api/disp?m=0 → 直接切电脑显示模式（不经过米家/巴法云）
static void handleApiDisp() {
  int m = http.arg("m").toInt();
  if (m < 0 || m >= (int)DISP_COUNT) {
    http.send(400, "text/plain; charset=utf-8", "模式无效（0~3）");
    return;
  }
  if (sendDisplay((uint8_t)m)) {
    http.send(200, "text/plain; charset=utf-8", String("已发送 ") + DISP_CMDS[m]);
  } else {
    http.send(500, "text/plain; charset=utf-8", "电脑地址没设置好，先在最下面填");
  }
}

// GET /api/cc?m=6&t=0&tm=0&d=0 → 发复杂指令
static void handleApiCc() {
  if (http.arg("m").length() == 0) {
    http.send(400, "text/plain; charset=utf-8", "缺少 mode");
    return;
  }
  uint16_t mode   = (uint16_t)constrain(http.arg("m").toInt(), 0L, 200L);
  uint16_t type   = (uint16_t)constrain(http.arg("t").length()  ? http.arg("t").toInt()  : 0L, 0L, 10L);
  uint16_t timev  = (uint16_t)constrain(http.arg("tm").length() ? http.arg("tm").toInt() : 0L, 0L, 3600L);
  uint16_t delayv = (uint16_t)constrain(http.arg("d").length()  ? http.arg("d").toInt()  : 0L, 0L, 60000L);

  if (sendComplex(mode, type, timev, delayv)) {
    http.send(200, "text/plain; charset=utf-8", String("已发送 mode=") + mode);
  } else {
    http.send(500, "text/plain; charset=utf-8", "发送失败");
  }
}

// GET /api/setpc?ip=192.168.1.100 → 改电脑 IP 并存进 flash
static void handleApiSetPc() {
  String s = http.arg("ip");
  IPAddress t;
  if (!t.fromString(s)) {
    http.send(400, "text/plain; charset=utf-8", "IP 格式不对");
    return;
  }
  prefs.putString("pcip", s);      // 写进 NVS，断电不丢
  pcIp = t;
  Serial.printf("[CFG] PC IP 改为 %s\n", pcIp.toString().c_str());
  http.send(200, "text/plain; charset=utf-8", String("已保存 ") + s);
}

// 其它路径一律 404
static void handleNotFound() {
  http.send(404, "text/plain; charset=utf-8", "404");
}

// 注册路由并启动服务器
static void webBegin() {
  http.on("/",           handleRoot);
  http.on("/api/status", handleStatus);
  http.on("/api/key",    handleApiKey);
  http.on("/api/cmd",    handleApiCmd);
  http.on("/api/power",  handleApiPower);
  http.on("/api/disp",   handleApiDisp);
  http.on("/api/cc",     handleApiCc);
  http.on("/api/setpc",  handleApiSetPc);
  http.onNotFound(handleNotFound);
  http.begin();
  webStarted = true;
  Serial.printf("[WEB] 打开 http://%s/ 即可遥控\n", WiFi.localIP().toString().c_str());
}


// ===========================================================================
//  第 16 部分：setup() —— 上电后只跑一次
// ===========================================================================

void setup() {
  Serial.begin(115200);      // 串口 115200（监视器要设成一样）
  delay(300);                // 等串口稳定
  Serial.println();
  Serial.println("=== XGIMI H3S v2.0 (ESP32 + bemfa + web) ===");

  // 投影 IP
  if (!projIp.fromString(PROJECTOR_IP)) {
    Serial.println("[CFG] PROJECTOR_IP 格式错误");
  }

  // ★v2★ 电脑 IP：优先读 flash 里保存的，没有就用 secrets.h 的默认值
  prefs.begin("dsh", false);                       // 打开命名空间 "dsh"
  String saved = prefs.getString("pcip", "");      // 读上次保存的值
  String useIp = saved.length() ? saved : String(PC_IP);
  if (!pcIp.fromString(useIp)) {
    Serial.println("[CFG] PC_IP 格式错误，显示模式功能不可用");
  }
  Serial.printf("[CFG] PC IP = %s%s\n", pcIp.toString().c_str(),
                saved.length() ? " (来自 flash)" : " (来自 secrets.h)");

  // 蓝牙：先组装广播包（顺带校验 token），再初始化硬件
  bleTokenOk = buildAdvPackets();
  if (!bleTokenOk) Serial.println("[CFG] BLE token 无效，开机功能不可用");

  bleReady = bleInit();
  if (!bleReady) Serial.println("[CFG] BLE 初始化失败，开机功能不可用");

  // ---- 连接 Wi-Fi ----
  WiFi.mode(WIFI_STA);        // 客户端模式（像手机连路由器）
  WiFi.setSleep(false);       // 关掉 Wi-Fi 省电休眠，降低延迟
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {  // 最多等 20 秒
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] ok, ip=");
    Serial.println(WiFi.localIP());     // ★这个 IP 就是网页地址★
  } else {
    Serial.println("[WiFi] timeout, 稍后自动重连");
  }

  // ---- MQTT ----
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(30);      // 心跳 30 秒
  mqtt.setBufferSize(256);    // 缓冲区，默认 128 有时不够

  udp.begin(0);               // 启动 UDP，本地端口 0 = 系统随便分配

  if (STATUS_LED >= 0) pinMode(STATUS_LED, OUTPUT);

  // ★v2★ 启动网页服务器（Wi-Fi 连上才能拿到 IP 并对外服务）
  if (WiFi.status() == WL_CONNECTED) {
    webBegin();
  } else {
    Serial.println("[WEB] Wi-Fi 没连上，重连成功后会自动启动网页");
  }
  wifiWasUp = (WiFi.status() == WL_CONNECTED);   // 记下初始状态

  logState("start");
}


// ===========================================================================
//  第 17 部分：loop() —— 主循环，永远重复执行
//  ---------------------------------------------------------------------------
//  关键：这里没有"长时间等待"。所有"等 8 秒"都是比较时间戳实现的。
//  一旦用 delay() 干等，串口命令、MQTT、网页全会卡住。
// ===========================================================================

void loop() {
  // ---- 1. 串口调试命令（115200，敲单个字符，不用回车）----
  //   o = 开机   f = 关机   w = 只发广播   s = 打印状态
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
                    (int)state, WiFi.localIP().toString().c_str(), mqtt.connected() ? 1 : 0,
                    bleReady ? 1 : 0, bleAdvertising ? 1 : 0);
      Serial.printf("[STATE] proj=%s pc=%s:%u web=http://%s/\n",
                    PROJECTOR_IP, pcIp.toString().c_str(), (unsigned)PC_PORT,
                    WiFi.localIP().toString().c_str());
    }
  }

  // ---- 2. ★v2★ 处理网页请求（必须经常调用，否则网页打不开）----
  // 注意：一定要先判断 webStarted。WebServer 在 begin() 之前内部对象是空的，
  // 直接调用 handleClient() 会访问空指针导致重启。
  if (webStarted) http.handleClient();

  // ---- 3. Wi-Fi 断了自动重连；连上后确保网页服务可用 ----
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiAt > 10000) {   // 每 10 秒试一次
      wifiAt = millis();
      Serial.println("[WiFi] reconnect");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    wifiWasUp = false;
  } else {
    if (!webStarted) {
      webBegin();                      // 开机时没连上 Wi-Fi，在这里补启动
    } else if (!wifiWasUp) {
      // Wi-Fi 断过又连上了：底层监听套接字可能已经失效，重启一次网页服务
      http.stop();
      http.begin();
      Serial.printf("[WEB] Wi-Fi 已恢复，网页服务重启：http://%s/\n",
                    WiFi.localIP().toString().c_str());
    }
    wifiWasUp = true;
  }

  // ---- 3.5 处理投影控制通道上推来的帧（不读的话缓冲区会越积越多）----
  pjPoll();

  // ---- 4. 维持 MQTT ----
  mqttEnsure();     // 断了就重连（5 秒节流）
  mqtt.loop();      // 必须常调：负责收消息和发心跳

  // ---- 4.5 定时重新订阅（自愈）----
  // 为什么需要：订阅只在"连上那一刻"发生一次。如果某个主题是在那之后才创建的，
  // 设备就永远收不到它；同理某次订阅被服务器丢掉也不会自动恢复。
  // 每 60 秒重订阅一遍，代价是 5 个小包，换来"新建主题不用重启板子"。
  if (mqtt.connected() && millis() - resubAt > 60000) {
    resubAt = millis();
    subscribeAll(false);       // false = 只在失败时打日志，平时不刷屏
  }

  // ---- 5. 广播到时间就停 ----
  if (bleAdvertising && millis() > bleStopAt) {
    esp_ble_gap_stop_advertising();
    bleAdvertising = false;
    Serial.println("[BLE] stop");
  }

  // ---- 6. 执行"留言"里的命令 ----
  if (cmdOn)  { cmdOn  = false; doPowerOn();  }
  if (cmdOff) { cmdOff = false; doPowerOff(); }

  // ---- 7. ★v2★ 执行"显示模式"留言 ----
  handleDispPending();

  // ---- 8. 重连成功后补报一次状态 ----
  if (needPublish && mqtt.connected()) {
    needPublish = false;
    if (state == ST_ON)        publishState(true);
    else if (state == ST_OFF)  publishState(false);
  }

  // ---- 9. 定时轮询投影状态 ----
  // 开机中每 2 秒一次（想尽快知道起来没），其它每 8 秒一次
  uint32_t interval = (state == ST_BOOTING) ? 2000 : POLL_MS;
  if (millis() - pollAt > interval) {
    pollAt = millis();
    pollState();
  }

  // ---- 10. 状态灯 ----
  // 常亮 = 已开机；快闪 = 开机中；慢闪 = 关机/未知
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
