#pragma once

// 复制本文件为 secrets.h，填入你自己的值。
// secrets.h 已在 .gitignore 中，不会被提交。

#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"
#define PROJECTOR_IP    "192.168.1.50"
#define BLE_TOKEN_HEX   "YOUR_32_HEX_TOKEN"
#define BEMFA_UID       "YOUR_BEMFA_PRIVATE_KEY"
#define BEMFA_TOPIC     "touying001"

// ---- v2.0 新增：电脑的局域网 IP ----
// 用来把"切换显示模式"的指令发给电脑上的 display_switch.py。
// 也可以不写这两行：固件里带了默认值，而且可以在网页上改（会存进 flash）。
// 建议在路由器里给电脑做 DHCP 静态绑定，否则 IP 变了就发不到。
#define PC_IP           "192.168.1.100"
#define PC_PORT         54321
