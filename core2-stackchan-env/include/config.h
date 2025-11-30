#pragma once

// ===== Wi-Fi設定 =====
// StickP2のSoftAPに接続
const char* WIFI_SSID     = "STACKCHAN";
const char* WIFI_PASSWORD = "12345678";

// ===== MQTTブローカー設定 =====
// StickP2がブローカー(SoftAP標準IP)
const char* MQTT_HOST = "192.168.4.1";
uint16_t    MQTT_PORT = 1883;

// StickP2がpublishするトピック
const char* MQTT_TOPIC_ENV = "env/json";

// 任意:Core2側の状態をpublishしたいとき用(後で使う)
const char* MQTT_TOPIC_STATE = "home/stackchan1/state";

// ===== 猫耳LED (わししさんNekomimi) =====
// LED数と接続ピン(Core2 AWS + GO BOTTOM2 の例:GPIO 26)
const uint8_t NEKOMIMI_LED_PIN  = 26;
const uint16_t NEKOMIMI_LED_NUM = 18;

// ===== しきい値(初期値・設定画面で変更可能) =====
const float TEMP_HOT_THRESHOLD    = 27.0f;  // 仕様書の初期値
const float TEMP_COLD_THRESHOLD   = 18.0f;
const float HUMID_HUMID_THRESHOLD = 70.0f;
const float HUMID_DRY_THRESHOLD   = 30.0f;
