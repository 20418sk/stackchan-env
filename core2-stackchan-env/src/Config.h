#ifndef CONFIG_H
#define CONFIG_H

// WiFi設定 (StickP2のSoftAP)
#define WIFI_SSID "StickP2-AP"
#define WIFI_PASSWORD "12345678"

// MQTT設定 (StickP2のMQTTブローカー)
#define MQTT_HOST "192.168.4.1"  // StickP2のAPのIPアドレス
#define MQTT_PORT 1883
#define MQTT_TOPIC_ENV "sensor/env"

// NeoPixel LED設定 (猫耳)
#define NEKOMIMI_LED_PIN 25  // Core2のGPIOピン番号(環境に合わせて変更)
#define NEKOMIMI_LED_NUM 10  // LEDの個数(環境に合わせて変更)

#endif // CONFIG_H
