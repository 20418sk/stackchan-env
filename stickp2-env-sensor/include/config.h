#pragma once

// ===== Wi-Fi =====
constexpr const char* WIFI_SSID     = ".HECT_freeWIFI_01";
constexpr const char* WIFI_PASSWORD = "freehect";

// ===== MQTT =====
constexpr const char* MQTT_HOST = "broker.emqx.io";
constexpr uint16_t    MQTT_PORT = 1883;

// publish先トピック
constexpr const char* MQTT_TOPIC_ENV = "home/env/stackchan1";

// 計測間隔（ミリ秒）
constexpr uint32_t MEASURE_INTERVAL_MS = 20000;  // 20秒ごと
