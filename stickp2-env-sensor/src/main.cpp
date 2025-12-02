#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <math.h>

#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedENV.h>

// ================================================================
//  1. 設定・型定義 / グローバル変数
// ================================================================

// ===== Wi-Fi 設定 =====
// → Core2 側の SoftAP と合わせる
const char* WIFI_SSID     = "Core2EnvAP";
const char* WIFI_PASSWORD = "m5password";

// ===== MQTT 設定 =====
// → Core2 側の SoftAP IP & トピックと合わせる
const char*   MQTT_SERVER = "192.168.4.1";
const uint16_t MQTT_PORT  = 1883;
const char*   MQTT_TOPIC  = "home/env/stackchan1";

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ===== ENV HAT III (Unit ENV3) =====
m5::unit::UnitUnified Units;
m5::unit::UnitENV3    env3;

auto& sht30   = env3.sht30;    // 温湿度センサ
auto& qmp6988 = env3.qmp6988;  // 気圧センサ

// ===== 環境計測値をまとめる struct =====
struct EnvReading {
    float temperature;  // ℃
    float humidity;     // %
    float pressure;     // hPa
    float altitude;     // m
    bool  valid;        // 有効な値を持っているか
};

EnvReading g_env = {NAN, NAN, NAN, NAN, false};

// 画面レイアウト用（1行の高さ）
const int16_t LINE_HEIGHT = 20;

// ================================================================
//  2. センサ関連ユーティリティ（高度計算）
// ================================================================

// ===== 高度計算（気圧→高度） =====
float calcAltitude(const float pressurePa, const float seaLevelhPa = 1013.25f) {
    float p_hPa = pressurePa * 0.01f;  // Pa → hPa
    return 44330.0f * (1.0f - powf(p_hPa / seaLevelhPa, 0.1903f));
}

// ================================================================
//  3. 通信層：Wi-Fi 接続 / MQTT 再接続
// ================================================================

// ===== Wi-Fi 接続 =====
void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    M5.Display.fillScreen(BLACK);
    M5.Display.setCursor(0, 0);
    M5.Display.setTextSize(2);
    M5.Display.println("WiFi connecting...");

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        M5.Display.print(".");
    }

    M5.Display.println("\nWiFi connected");
    M5.Display.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    delay(1000);
}

// ===== MQTT 再接続 =====
void reconnectMQTT() {
    while (!mqttClient.connected()) {
        M5.Display.fillRect(0, LINE_HEIGHT * 4, M5.Display.width(), LINE_HEIGHT, BLACK);
        M5.Display.setCursor(0, LINE_HEIGHT * 4);
        M5.Display.setTextSize(1);
        M5.Display.print("MQTT connecting...");

        String clientId = "StickP2-" + String((uint32_t)ESP.getEfuseMac(), HEX);

        if (mqttClient.connect(clientId.c_str())) {
            M5.Display.fillRect(0, LINE_HEIGHT * 4, M5.Display.width(), LINE_HEIGHT, BLACK);
            M5.Display.setCursor(0, LINE_HEIGHT * 4);
            M5.Display.print("MQTT connected   ");
        } else {
            M5.Display.fillRect(0, LINE_HEIGHT * 4, M5.Display.width(), LINE_HEIGHT, BLACK);
            M5.Display.setCursor(0, LINE_HEIGHT * 4);
            M5.Display.printf("MQTT fail rc=%d", mqttClient.state());
            delay(2000);
        }
    }
}

// ================================================================
//  4. センサ層：ENV HAT III からの値取得
// ================================================================

// ===== 環境センサ値の更新（センサー担当） =====
void updateEnv(EnvReading& env) {
    // UnitUnified の裏側で I2C 読み取り
    Units.update();

    bool updated = false;

    // SHT30: 温度・湿度
    if (sht30.updated()) {
        env.temperature = sht30.temperature();
        env.humidity    = sht30.humidity();
        updated         = true;
    }

    // QMP6988: 気圧
    if (qmp6988.updated()) {
        float pPa = qmp6988.pressure();
        env.pressure = pPa * 0.01f;      // hPa
        env.altitude = calcAltitude(pPa);
        updated      = true;
    }

    if (updated) {
        env.valid = true;
    }
}

// ================================================================
//  5. 画面描画層：StickP2 ディスプレイ表示
// ================================================================

// ===== 画面表示担当（差分だけ描画してチカチカ防止） =====
void drawEnv(const EnvReading& env) {
    if (!env.valid) {
        static bool shown = false;
        if (!shown) {
            shown = true;
            M5.Display.setTextSize(2);
            M5.Display.fillScreen(BLACK);
            M5.Display.setCursor(0, 0);
            M5.Display.print("No data yet...");
        }
        return;
    }

    static EnvReading prev = {NAN, NAN, NAN, NAN, false};
    const float DELTA = 0.05f;  // このくらい変わったら描画し直す

    bool needRedrawTemp = isnan(prev.temperature) ||
                          fabs(env.temperature - prev.temperature) > DELTA;
    bool needRedrawHum  = isnan(prev.humidity)    ||
                          fabs(env.humidity    - prev.humidity)    > DELTA;
    bool needRedrawPres = isnan(prev.pressure)    ||
                          fabs(env.pressure    - prev.pressure)    > DELTA;
    bool needRedrawAlt  = isnan(prev.altitude)    ||
                          fabs(env.altitude    - prev.altitude)    > DELTA;

    M5.Display.setTextSize(2);

    // 1行目: 温度
    if (needRedrawTemp) {
        M5.Display.fillRect(0, 0, 200, LINE_HEIGHT, BLACK);
        M5.Display.setCursor(0, 0);
        M5.Display.printf("Temp: %.2f C", env.temperature);
    }

    // 2行目: 湿度
    if (needRedrawHum) {
        M5.Display.fillRect(0, LINE_HEIGHT, 200, LINE_HEIGHT, BLACK);
        M5.Display.setCursor(0, LINE_HEIGHT);
        M5.Display.printf("Hum : %.2f %%", env.humidity);
    }

    // 3行目: 気圧
    if (needRedrawPres) {
        M5.Display.fillRect(0, LINE_HEIGHT * 2, 200, LINE_HEIGHT, BLACK);
        M5.Display.setCursor(0, LINE_HEIGHT * 2);
        M5.Display.printf("Pres: %.2f hPa", env.pressure);
    }

    // 4行目: 高度
    if (needRedrawAlt) {
        M5.Display.fillRect(0, LINE_HEIGHT * 3, 200, LINE_HEIGHT, BLACK);
        M5.Display.setCursor(0, LINE_HEIGHT * 3);
        M5.Display.printf("Alt : %.1f m", env.altitude);
    }

    prev = env;
}

// ================================================================
//  6. Publish 間隔管理（タイミング制御）
// ================================================================

// ===== Publish のタイミング管理 =====
const unsigned long PUBLISH_INTERVAL_MS = 2000;
unsigned long g_lastPublish = 0;

bool shouldPublish() {
    unsigned long now = millis();
    if (now - g_lastPublish >= PUBLISH_INTERVAL_MS) {
        g_lastPublish = now;
        return true;
    }
    return false;
}

// ================================================================
//  7. MQTT 送信層：Publish 処理
// ================================================================

// ===== MQTT 送信担当 =====
void publishEnv(const EnvReading& env) {
    if (!env.valid) {
        return;
    }

    if (!mqttClient.connected()) {
        reconnectMQTT();
    }

    char payload[64];
    snprintf(payload, sizeof(payload), "%.2f,%.2f,%.2f",
             env.temperature, env.humidity, env.pressure);

    Serial.print("MQTT publish: ");
    Serial.println(payload);

    mqttClient.publish(MQTT_TOPIC, payload);

    // 画面の一番下の行だけ軽く更新
    int16_t w = M5.Display.width();
    int16_t h = M5.Display.height();
    M5.Display.fillRect(0, h - LINE_HEIGHT, w, LINE_HEIGHT, BLACK);
    M5.Display.setCursor(0, h - LINE_HEIGHT);
    M5.Display.setTextSize(1);
    M5.Display.print("sent");
}

// ================================================================
//  8. ライフサイクル：setup()
// ================================================================
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    Serial.begin(115200);
    delay(200);

    M5.Display.setRotation(1);
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.setTextSize(2);

    // ENV HAT III の I2C 初期化 (StickC Plus2 HATピン: SDA=0, SCL=26)
    Wire.begin(0 /*SDA*/, 26 /*SCL*/, 400000 /*Hz*/);

    if (!Units.add(env3, Wire) || !Units.begin()) {
        M5.Display.fillScreen(RED);
        M5.Display.setCursor(0, 0);
        M5.Display.println("ENV HAT3 init ERR");
        Serial.println("Failed to init ENV HAT III");
        while (true) {
            delay(1000);
        }
    }

    // Wi-Fi & MQTT 初期化
    connectWiFi();
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);

    M5.Display.fillScreen(BLACK);
    M5.Display.setCursor(0, 0);
    M5.Display.println("StickP2 Ready");
    delay(1000);

    // 初期画面クリア
    M5.Display.fillScreen(BLACK);
}

// ================================================================
//  9. ライフサイクル：loop()
// ================================================================
void loop() {
    M5.update();

    // センサー更新
    updateEnv(g_env);

    // MQTT 接続維持
    if (!mqttClient.connected()) {
        reconnectMQTT();
    }
    mqttClient.loop();

    // 描画は一定間隔だけ（チカチカ防止）
    static unsigned long lastDraw = 0;
    const unsigned long DRAW_INTERVAL_MS = 500;  // 0.5秒ごとに画面更新

    unsigned long now = millis();
    if (now - lastDraw >= DRAW_INTERVAL_MS) {
        lastDraw = now;
        drawEnv(g_env);
    }

    // 一定間隔で Publish
    if (shouldPublish()) {
        publishEnv(g_env);
    }

    delay(10);
}
