#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PicoMQTT.h>
#include <M5Unified.h>
#include <Avatar.h>
#include <LittleFS.h>
#include <math.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Servo.h>

using namespace m5avatar;

// ================================================================
//  1. 設定・型定義 / グローバル変数
// ================================================================

// ======================================================================
//  サーボ（首）設定
// ======================================================================
Servo servoX;   // 左右（ヨー）
Servo servoY;   // 上下（ピッチ）
bool  g_servoAttached = false;  // attach 済みかどうか

// サーボの接続ピン（Core2 カスタムシールド前提）
constexpr int SERVO_X_PIN = 33;
constexpr int SERVO_Y_PIN = 32;

// 基準角度（センター位置）と左右スイング幅
constexpr int SERVO_X_CENTER    = 90;
constexpr int SERVO_Y_CENTER    = 90;
constexpr int SERVO_X_AMPLITUDE = 15;   // 左右のふり幅

// 首の上下用：現在角度・目標角度・ポーズ切り替えタイミング
float         g_servoYCurrent = SERVO_Y_CENTER;
float         g_servoYTarget  = SERVO_Y_CENTER;
unsigned long g_nextPoseChangeMs = 0;

// ======================================================================
//  SoftAP 設定
// ======================================================================
const char* AP_SSID     = "Core2EnvAP";
const char* AP_PASSWORD = "m5password";

// ======================================================================
//  MQTT 設定
// ======================================================================
const uint16_t MQTT_PORT  = 1883;
const char*    MQTT_TOPIC = "home/env/stackchan1";  // StickP2側と合わせる

// ======================================================================
//  LittleFS ファイルパス
// ======================================================================
const char* LOG_FILE_PATH    = "/logs.csv";
const char* CONFIG_FILE_PATH = "/config.txt";

// ======================================================================
//  MQTT ブローカ / HTTP サーバ / Avatar
// ======================================================================
PicoMQTT::Server mqtt;
WebServer        server(80);
Avatar           avatar;

// ======================================================================
//  起動フェーズ管理
// ======================================================================
enum class BootPhase {
    QR,       // QRコード表示モード
    Avatar    // Avatarモード
};
BootPhase g_bootPhase = BootPhase::QR;

// ======================================================================
//  QRサブページ管理
// ======================================================================
enum class QRSubPage {
    Wifi,
    Url
};
QRSubPage g_qrPage = QRSubPage::Wifi;

// ======================================================================
//  受信した環境値
// ======================================================================
struct EnvReading {
    float temperature;  // ℃（オフセット適用後）
    float humidity;     // %
    float pressure;     // hPa
    bool  valid;        // 有効データを受信済みか
};

EnvReading g_env = {NAN, NAN, NAN, false};

// ======================================================================
//  温度オフセット（補正値）
// ======================================================================
float g_tempOffset = 0.0f;

// ======================================================================
//  ログ管理（メモリ上）
//  - ログ毎に「記録日時文字列」を持つ
// ======================================================================
struct EnvLogEntry {
    float temperature;
    float humidity;
    float pressure;
    char  datetime[20];   // "YYYY/MM/DD HH:MM:SS" + 終端 = 20バイト
};

constexpr size_t LOG_CAPACITY = 32;
EnvLogEntry g_logs[LOG_CAPACITY];
size_t      g_logCount    = 0;
size_t      g_logSelected = 0;

// 吹き出しON/OFF
bool g_showSpeech = true;

// ======================================================================
//  LED（本体＋猫耳）設定
// ======================================================================
// Core2 底面 SK6812（10個）
static const int BODY_LED_PIN   = 25;
static const int BODY_LED_COUNT = 10;

// 猫耳 LED（左右9個ずつ = 18個）
static const int EARS_LED_PIN   = 26;   // PortB OUT などに接続
static const int EARS_LED_COUNT = 18;

Adafruit_NeoPixel bodyStrip(BODY_LED_COUNT, BODY_LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel earsStrip(EARS_LED_COUNT, EARS_LED_PIN, NEO_GRB + NEO_KHZ800);

bool g_ledInited = false;
bool g_ledWasOn  = false;   // 直前フレームでLEDが点灯していたか？

// ======================================================================
//  表情変化／鳴き声制御用
// ======================================================================
// 直前の表情（表情変化検出用）
Expression g_lastExpression   = Expression::Neutral;
bool       g_exprInitialized  = false;

// 「次のループで悲鳴を鳴らしてほしい」フラグ
bool       g_requestScream    = false;

// ======================================================================
//  プロトタイプ宣言
// ======================================================================
void updateAvatarExpression();
void updateSpeech();
bool  saveOffsetToFS();
bool  rewriteLogsToFS();
void  startMQTTBroker();
void  enterAvatarMode();
void  showWifiQRScreen();
void  showUrlQRScreen();
void  initLeds();
void  updateLedsForTemp();
void  playScreamSound();
void  initServo();
void  updateServoIdle();
void  getCurrentDatetimeString(char* buf, size_t len);
void  handleSetTime();
Expression getExpressionForTemp(float t);   // 温度→表情 ヘルパー

// ================================================================
//  2. 共通ユーティリティ（エラー／警告表示）
// ================================================================

// ======================================================================
//  致命的エラー表示
// ======================================================================
[[noreturn]] void showFatalAndWait(const char* msg) {
    M5.Display.fillScreen(RED);
    M5.Display.setTextColor(WHITE, RED);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(0, 0);
    M5.Display.println("FATAL ERROR");
    M5.Display.setTextSize(1);
    M5.Display.println();
    M5.Display.println(msg);
    M5.Display.println();
    M5.Display.println("C: Restart");

    while (true) {
        M5.update();
        if (M5.BtnC.wasPressed()) {
            ESP.restart();
        }
        delay(50);
    }
}

// ======================================================================
//  警告表示（処理継続）
// ======================================================================
void showWarning(const char* msg) {
    int16_t w = M5.Display.width();
    int16_t h = M5.Display.height();
    int barH  = 24;

    M5.Display.fillRect(0, h - barH, w, barH, YELLOW);
    M5.Display.setTextColor(BLACK, YELLOW);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(4, h - barH + 4);
    M5.Display.print("WARN: ");
    M5.Display.print(msg);
    M5.Display.setTextColor(WHITE, BLACK);
}

// ================================================================
//  3. I/O ユーティリティ（LED）
// ================================================================

// ======================================================================
//  LEDユーティリティ
// ======================================================================
void setAllLedsColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_ledInited) return;

    for (int i = 0; i < BODY_LED_COUNT; ++i) {
        bodyStrip.setPixelColor(i, bodyStrip.Color(r, g, b));
    }
    bodyStrip.show();

    for (int i = 0; i < EARS_LED_COUNT; ++i) {
        earsStrip.setPixelColor(i, earsStrip.Color(r, g, b));
    }
    earsStrip.show();
}

void turnOffAllLeds() {
    setAllLedsColor(0, 0, 0);
}

// ================================================================
//  4. 表情・LED・サウンド制御
// ================================================================

// ======================================================================
//  温度 → Expression 判定（共通ロジック）
// ======================================================================
Expression getExpressionForTemp(float t) {
    if (t < 18.0f) {
        return Expression::Sad;
    } else if (t < 22.0f) {
        return Expression::Neutral;
    } else if (t <= 26.0f) {
        return Expression::Happy;
    } else if (t <= 30.0f) {
        return Expression::Doubt;
    } else {
        return Expression::Angry;
    }
}

// ======================================================================
//  温度に応じて LED 色切り替え（表情と連動）
//   Sad    = 青
//   Neutral= 水色
//   Doubt  = ピンク
//   Angry  = 赤
//   Happy  = 消灯
// ======================================================================
void updateLedsForTemp() {
    if (!g_ledInited) return;

    if (!g_env.valid) {
        turnOffAllLeds();
        g_ledWasOn = false;
        return;
    }

    // 温度から表情を取得（Avatar と同じロジック）
    Expression expr = getExpressionForTemp(g_env.temperature);

    uint8_t r = 0, g = 0, b = 0;
    bool shouldBeOn = true;

    switch (expr) {
        case Expression::Sad:
            // 青
            r = 0;   g = 0;   b = 160;
            break;

        case Expression::Neutral:
            // 水色（シアン寄り）
            r = 80;  g = 160; b = 160;
            break;

        case Expression::Doubt:
            // ピンク
            r = 200; g = 80;  b = 160;
            break;

        case Expression::Angry:
            // 赤
            r = 200; g = 40;  b = 40;
            break;

        case Expression::Happy:
        default:
            // 快適ゾーン：耳は光らせない
            turnOffAllLeds();
            shouldBeOn = false;
            break;
    }

    if (shouldBeOn) {
        setAllLedsColor(r, g, b);
    }

    // ★ 消灯状態 → 点灯状態に変わったタイミングでだけ
    //    「鳴いてほしい」フラグを立てる（ここでは音は鳴らさない）
    if (shouldBeOn && !g_ledWasOn) {
        g_requestScream = true;
    }

    g_ledWasOn = shouldBeOn;
}

// ======================================================================
//  人間が「助けたくなる」弱々しい電子泣き声
// ======================================================================
void playScreamSound() {

    // ① 小さく呼びかける「ひっ…」
    M5.Speaker.tone(1800, 50);   // か細い高め
    delay(40);

    // ② 震える弱音
    for (int i = 0; i < 5; i++) {
        int f = 1600 + (int)(sin(i * 1.1f) * 180.0f);  // 揺れを大きめに
        M5.Speaker.tone(f, 40);
        delay(25);
    }

    // ③ 今にも涙がこぼれそうな伸び
    M5.Speaker.tone(2200, 280);

    // ④ 息がしぼむ
    M5.Speaker.tone(1300, 60);
}

// ================================================================
//  5. データ層：設定・RTC・ログ（LittleFS）
// ================================================================

// ======================================================================
//  LittleFS: オフセットの読み書き
// ======================================================================
bool loadOffsetFromFS() {
    if (!LittleFS.exists(CONFIG_FILE_PATH)) return false;
    File f = LittleFS.open(CONFIG_FILE_PATH, FILE_READ);
    if (!f) return false;

    String line = f.readStringUntil('\n');
    line.trim();
    f.close();
    if (line.length() == 0) return false;

    g_tempOffset = line.toFloat();
    return true;
}

bool saveOffsetToFS() {
    File f = LittleFS.open(CONFIG_FILE_PATH, FILE_WRITE);
    if (!f) return false;
    f.printf("%.2f\n", g_tempOffset);
    f.close();
    return true;
}

// ======================================================================
//  RTC → "YYYY/MM/DD HH:MM:SS" に変換
// ======================================================================
void getCurrentDatetimeString(char* buf, size_t len) {
    auto dt = M5.Rtc.getDateTime();  // rtc_datetime_t

    snprintf(buf, len,
             "%04d/%02d/%02d %02d:%02d:%02d",
             (int)dt.date.year,
             (int)dt.date.month,
             (int)dt.date.date,
             (int)dt.time.hours,
             (int)dt.time.minutes,
             (int)dt.time.seconds);
}

// ======================================================================
//  LittleFS: ログの読み書き
//   CSV: temperature,humidity,pressure,datetime
// ======================================================================
bool loadLogsFromFS() {
    g_logCount    = 0;
    g_logSelected = 0;

    if (!LittleFS.exists(LOG_FILE_PATH)) return false;
    File f = LittleFS.open(LOG_FILE_PATH, FILE_READ);
    if (!f) return false;

    while (f.available() && g_logCount < LOG_CAPACITY) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        float t, h, p;
        char  dtstr[20] = {0};

        if (sscanf(line.c_str(), "%f,%f,%f,%19[^\n]",
                   &t, &h, &p, dtstr) == 4) {
            EnvLogEntry e;
            e.temperature = t;
            e.humidity    = h;
            e.pressure    = p;
            strncpy(e.datetime, dtstr, sizeof(e.datetime));
            e.datetime[sizeof(e.datetime) - 1] = '\0';
            g_logs[g_logCount++] = e;
        }
    }
    f.close();

    if (g_logCount > 0) {
        g_logSelected = g_logCount - 1;
    }
    return (g_logCount > 0);
}

bool rewriteLogsToFS() {
    File f = LittleFS.open(LOG_FILE_PATH, FILE_WRITE);
    if (!f) return false;

    for (size_t i = 0; i < g_logCount; ++i) {
        const auto& e = g_logs[i];
        f.printf("%.1f,%.1f,%.1f,%s\n",
                 e.temperature,
                 e.humidity,
                 e.pressure,
                 e.datetime);
    }
    f.close();
    return true;
}

bool appendLogToFS(const EnvLogEntry& e) {
    File f = LittleFS.open(LOG_FILE_PATH, FILE_APPEND);
    if (!f) return false;

    f.printf("%.1f,%.1f,%.1f,%s\n",
             e.temperature,
             e.humidity,
             e.pressure,
             e.datetime);
    f.close();
    return true;
}

// ======================================================================
//  ログ追加（変化が小さいときはスキップ）
// ======================================================================
void addLogEntry(const EnvReading& env) {
    if (!env.valid) return;

    if (g_logCount > 0) {
        const auto& last = g_logs[g_logCount - 1];
        if (fabsf(env.temperature - last.temperature) < 0.2f &&
            fabsf(env.humidity    - last.humidity)    < 1.0f &&
            fabsf(env.pressure    - last.pressure)    < 0.5f) {
            return;
        }
    }

    EnvLogEntry e;
    e.temperature = env.temperature;
    e.humidity    = env.humidity;
    e.pressure    = env.pressure;
    getCurrentDatetimeString(e.datetime, sizeof(e.datetime));

    if (g_logCount < LOG_CAPACITY) {
        g_logs[g_logCount++] = e;
    } else {
        for (size_t i = 1; i < LOG_CAPACITY; ++i) {
            g_logs[i - 1] = g_logs[i];
        }
        g_logs[LOG_CAPACITY - 1] = e;
    }

    g_logSelected = (g_logCount > 0) ? (g_logCount - 1) : 0;

    appendLogToFS(e);
}

// ======================================================================
//  ログ削除 / 全削除
// ======================================================================
void deleteLogAt(size_t index) {
    if (g_logCount == 0) return;
    if (index >= g_logCount) return;

    for (size_t i = index + 1; i < g_logCount; ++i) {
        g_logs[i - 1] = g_logs[i];
    }
    --g_logCount;

    if (g_logCount == 0) {
        g_logSelected = 0;
        LittleFS.remove(LOG_FILE_PATH);
    } else {
        if (g_logSelected >= g_logCount) {
            g_logSelected = g_logCount - 1;
        }
        rewriteLogsToFS();
    }
}

void clearAllLogs() {
    g_logCount    = 0;
    g_logSelected = 0;
    LittleFS.remove(LOG_FILE_PATH);
}

// ================================================================
//  6. I/O層：LED・サーボ・Avatar・サウンド
// ================================================================

// ======================================================================
//  LED 初期化
// ======================================================================
void initLeds() {
    bodyStrip.begin();
    earsStrip.begin();

    bodyStrip.setBrightness(40);
    earsStrip.setBrightness(40);

    turnOffAllLeds();
    g_ledInited = true;
}

// ======================================================================
//  サーボ初期化
// ======================================================================
void initServo() {
    if (g_servoAttached) return;

    servoX.setPeriodHertz(50);
    servoY.setPeriodHertz(50);
    servoX.attach(SERVO_X_PIN, 500, 2400);
    servoY.attach(SERVO_Y_PIN, 500, 2400);

    servoX.write(SERVO_X_CENTER);
    servoY.write(SERVO_Y_CENTER);

    g_servoYCurrent = SERVO_Y_CENTER;
    g_servoYTarget  = SERVO_Y_CENTER;
    g_nextPoseChangeMs = millis() + random(3000, 7000);

    g_servoAttached = true;
}

// ======================================================================
//  ボタン操作音
// ======================================================================
void playClickSound() {
    M5.Speaker.tone(1000, 40);
}

// ======================================================================
//  公式風 IDLE モーション
// ======================================================================
void updateServoIdle() {
    if (!g_servoAttached) return;

    unsigned long now = millis();

    const float PI_F    = 3.1415926f;
    const float PERIOD  = 4.5f;
    float t = now / 1000.0f;
    float s = sinf(2.0f * PI_F * t / PERIOD);  // -1〜1

    int yaw = SERVO_X_CENTER + (int)(SERVO_X_AMPLITUDE * s);
    yaw = constrain(yaw, 0, 180);
    servoX.write(yaw);

    if (now >= g_nextPoseChangeMs) {
        static const int offsets[] = { -15, -5, 0, 5, 10 };
        int idx = random(0, 5);
        int base = SERVO_Y_CENTER + offsets[idx];
        base = constrain(base, 40, 140);
        g_servoYTarget = base;

        unsigned long interval = (unsigned long)random(5000, 12001);
        g_nextPoseChangeMs = now + interval;
    }

    g_servoYCurrent += (g_servoYTarget - g_servoYCurrent) * 0.05f;
    int pitch = (int)(g_servoYCurrent + 0.5f);
    pitch = constrain(pitch, 0, 180);
    servoY.write(pitch);
}

// ======================================================================
//  Avatar 表情（温度→表情ヘルパーを使用）
//   表情が変わったタイミングで g_requestScream = true にする
// ======================================================================
void updateAvatarExpression() {
    if (!g_env.valid) {
        avatar.setExpression(Expression::Neutral);
        g_lastExpression  = Expression::Neutral;
        g_exprInitialized = true;
        return;
    }

    Expression newExpr = getExpressionForTemp(g_env.temperature);

    if (!g_exprInitialized) {
        // 起動直後は「変化」とみなさない（いきなり鳴かない）
        g_lastExpression  = newExpr;
        g_exprInitialized = true;
    } else if (newExpr != g_lastExpression) {
        // 表情が変わったタイミングでだけ鳴きリクエスト
        g_requestScream = true;
        g_lastExpression = newExpr;
    }

    avatar.setExpression(newExpr);
}

// ======================================================================
//  吹き出し
// ======================================================================
void updateSpeech() {
    if (!g_showSpeech) {
        avatar.setSpeechText("");
        return;
    }

    if (!g_env.valid) {
        avatar.setSpeechText("Waiting MQTT...");
        return;
    }

    char buf[200];
    snprintf(buf, sizeof(buf),
             "Temp: %.1fC  Hum: %.0f%%",
             g_env.temperature,
             g_env.humidity);

    avatar.setSpeechText(buf);
}

// ================================================================
//  7. 通信層：Wi-Fi / MQTT
// ================================================================

// ======================================================================
//  SoftAP 起動
// ======================================================================
bool startSoftAP() {
    WiFi.mode(WIFI_AP);

    IPAddress local_ip(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    if (!WiFi.softAPConfig(local_ip, gateway, subnet)) {
        return false;
    }

    bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD);
    if (!ok) {
        return false;
    }

    IPAddress ip = WiFi.softAPIP();
    Serial.println("[WiFi] SoftAP started");
    Serial.print("  SSID: "); Serial.println(AP_SSID);
    Serial.print("  PASS: "); Serial.println(AP_PASSWORD);
    Serial.print("  IP  : "); Serial.println(ip);
    return true;
}

// ======================================================================
//  MQTT ブローカ
// ======================================================================
void startMQTTBroker() {
    mqtt.subscribe("#", [](const char* topic, const char* payload) {
        if (strcmp(topic, MQTT_TOPIC) != 0) return;

        float t, h, p;
        if (sscanf(payload, "%f,%f,%f", &t, &h, &p) == 3) {
            float finalT = t + g_tempOffset;

            g_env.temperature = finalT;
            g_env.humidity    = h;
            g_env.pressure    = p;
            g_env.valid       = true;

            addLogEntry(g_env);
            updateAvatarExpression();  // ここではフラグを立てるだけ
            updateSpeech();
            updateLedsForTemp();       // ここでも必要ならフラグを立てる
        }
    });

    mqtt.begin();
    Serial.println("[MQTT] Broker started (PicoMQTT)");
}

// ================================================================
//  8. HTTP 層：Webコンソール・RTC設定
// ================================================================

// ======================================================================
//  HTTP: ルート（Webコンソール）
// ======================================================================
void handleRoot() {
    String html;
    html.reserve(4096);

    html += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<title>Stackchan Env Console</title>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<style>";
    html += "body{font-family:sans-serif;margin:8px;}";
    html += "table{border-collapse:collapse;width:100%;}";
    html += "th,td{border:1px solid #ccc;padding:4px;font-size:12px;}";
    html += "th{background:#eee;}";
    html += "a.btn{display:inline-block;margin:2px 4px;padding:4px 8px;border:1px solid #333;";
    html += "border-radius:4px;text-decoration:none;font-size:12px;}";
    html += "</style></head><body>";

    html += "<h2>Stackchan Env Console</h2>";

    // 現在値
    html += "<h3>Current</h3><ul>";
    if (!g_env.valid) {
        html += "<li>Waiting MQTT...</li>";
    } else {
        html += "<li>Temperature: " + String(g_env.temperature, 1) +
                " &deg;C (offset " + String(g_tempOffset, 1) + " &deg;C)</li>";
        html += "<li>Humidity: " + String(g_env.humidity, 0) + " %</li>";
        html += "<li>Pressure: " + String(g_env.pressure, 1) + " hPa</li>";
    }
    html += "</ul>";

    // RTC表示 + 設定リンク
    {
        char nowBuf[20];
        getCurrentDatetimeString(nowBuf, sizeof(nowBuf));

        html += "<h3>RTC Time</h3>";
        html += "<p>Current RTC: <b>";
        html += nowBuf;
        html += "</b></p>";
        html += "<p><a class='btn' href='/settime'>Set RTC Time</a></p>";
    }

    // オフセット操作
    html += "<h3>Offset</h3>";
    html += "<p>Temp offset: <b>" + String(g_tempOffset, 1) + " &deg;C</b></p>";
    html += "<p>";
    html += "<a class='btn' href='/offset?delta=-0.5'>-0.5 C</a>";
    html += "<a class='btn' href='/offset?delta=0.5'>+0.5 C</a>";
    html += "</p>";

    // ログ一覧
    html += "<h3>Logs</h3>";
    html += "<p>Total: " + String((int)g_logCount) + "</p>";

    html += "<table><tr>"
            "<th>#</th>"
            "<th>Datetime</th>"
            "<th>Temp</th>"
            "<th>Hum</th>"
            "<th>Press</th>"
            "<th>Action</th>"
            "</tr>";

    for (size_t i = 0; i < g_logCount; ++i) {
        const auto& e = g_logs[i];

        html += "<tr>";
        html += "<td>" + String((int)i) + "</td>";
        html += "<td>" + String(e.datetime) + "</td>";
        html += "<td>" + String(e.temperature, 1) + "</td>";
        html += "<td>" + String(e.humidity, 0)    + "</td>";
        html += "<td>" + String(e.pressure, 1)    + "</td>";
        html += "<td><a class='btn' href='/delete?index=" + String((int)i) +
                "'>Delete</a></td>";
        html += "</tr>";
    }

    html += "</table>";

    if (g_logCount > 0) {
        html += "<p><a class='btn' href='/clear'>Clear All Logs</a></p>";
    }

    html += "<hr><p>操作メモ：<br>"
            "- 起動直後は本体画面にQRコードが出ます。<br>"
            "- スマホでWi-Fi用QR → Web用QRの順に読むと、このページを開けます。<br>"
            "- Avatar画面でもこのページからオフセットとログ操作ができます。</p>";

    html += "</body></html>";

    server.send(200, "text/html", html);
}

// ======================================================================
//  HTTP: オフセット変更
// ======================================================================
void handleOffset() {
    if (!server.hasArg("delta")) {
        server.send(400, "text/plain", "delta param required");
        return;
    }
    float delta = server.arg("delta").toFloat();
    g_tempOffset += delta;
    saveOffsetToFS();

    if (g_env.valid && g_bootPhase == BootPhase::Avatar) {
        g_env.temperature += delta;
        updateAvatarExpression();
        updateSpeech();
        updateLedsForTemp();
    }

    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "Redirecting...");
}

// ======================================================================
//  HTTP: ログ削除 / 全削除
// ======================================================================
void handleDelete() {
    if (!server.hasArg("index")) {
        server.send(400, "text/plain", "index param required");
        return;
    }
    int idx = server.arg("index").toInt();
    if (idx < 0 || (size_t)idx >= g_logCount) {
        server.send(400, "text/plain", "invalid index");
        return;
    }

    deleteLogAt((size_t)idx);

    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "Redirecting...");
}

void handleClear() {
    clearAllLogs();
    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "Redirecting...");
}

// ======================================================================
//  HTTP: RTC 時刻設定 (/settime)
// ======================================================================
void handleSetTime() {
    // dt 無し → 設定フォームを表示
    if (!server.hasArg("dt")) {
        String html;
        html.reserve(2048);

        char nowBuf[20];
        getCurrentDatetimeString(nowBuf, sizeof(nowBuf));

        html += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
        html += "<title>Set RTC Time</title>";
        html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
        html += "<style>";
        html += "body{font-family:sans-serif;margin:8px;}";
        html += "input[type=text]{width:180px;}";
        html += "button{margin:4px 0;padding:4px 8px;}";
        html += "</style>";
        html += "<script>";
        html += "function pad(n){return n<10?'0'+n:n;}";
        html += "function setFromDeviceTime(){";
        html += "  var d=new Date();";
        html += "  var y=d.getFullYear();";
        html += "  var m=pad(d.getMonth()+1);";
        html += "  var dd=pad(d.getDate());";
        html += "  var hh=pad(d.getHours());";
        html += "  var mm=pad(d.getMinutes());";
        html += "  var ss=pad(d.getSeconds());";
        html += "  var s=y+'/'+m+'/'+dd+' '+hh+':'+mm+':'+ss;";
        html += "  var url='/settime?dt='+encodeURIComponent(s);";
        html += "  location.href=url;";
        html += "}";
        html += "</script>";
        html += "</head><body>";

        html += "<h2>Set RTC Time</h2>";
        html += "<p>現在のRTC: ";
        html += nowBuf;
        html += "</p>";

        html += "<h3>このスマホの時刻でセット</h3>";
        html += "<p><button onclick='setFromDeviceTime()'>";
        html += "Set RTC from this device time";
        html += "</button></p>";

        html += "<hr>";

        html += "<h3>手動入力でセット</h3>";
        html += "<form method='GET' action='/settime'>";
        html += "日時 (YYYY/MM/DD HH:MM:SS):<br>";
        html += "<input type='text' name='dt' value='";
        html += nowBuf;
        html += "'><br><br>";
        html += "<input type='submit' value='Set Time'>";
        html += "</form>";

        html += "<p><a href='/'>Back to Console</a></p>";
        html += "</body></html>";

        server.send(200, "text/html", html);
        return;
    }

    // dt 付きで来たとき
    String s = server.arg("dt");
    s.trim();

    int yyyy, mm, dd, HH, MM, SS;
    if (sscanf(s.c_str(), "%d/%d/%d %d:%d:%d",
               &yyyy, &mm, &dd, &HH, &MM, &SS) != 6) {
        server.send(400, "text/plain", "Invalid format. Use YYYY/MM/DD HH:MM:SS");
        return;
    }

    if (yyyy < 2000 || mm < 1 || mm > 12 || dd < 1 || dd > 31 ||
        HH < 0 || HH > 23 || MM < 0 || MM > 59 || SS < 0 || SS > 59) {
        server.send(400, "text/plain", "Invalid datetime value");
        return;
    }

    m5::rtc_date_t date;
    date.year    = (uint16_t)yyyy;
    date.month   = (uint8_t)mm;
    date.date    = (uint8_t)dd;
    date.weekDay = 0; // 未使用

    m5::rtc_time_t rtcTime;
    rtcTime.hours   = (uint8_t)HH;
    rtcTime.minutes = (uint8_t)MM;
    rtcTime.seconds = (uint8_t)SS;

    m5::rtc_datetime_t dt;
    dt.date = date;
    dt.time = rtcTime;

    M5.Rtc.setDateTime(dt);
    Serial.printf("[RTC] Set to %04d/%02d/%02d %02d:%02d:%02d\n",
                  yyyy, mm, dd, HH, MM, SS);

    server.sendHeader("Location", "/");
    server.send(303, "RTC updated. Redirecting...");
}

// ======================================================================
//  HTTP: NotFound
// ======================================================================
void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

// ================================================================
//  9. 画面表示：QRコード（Wi-Fi / Webコンソール）
// ================================================================

// ======================================================================
//  QR画面（Wi-Fi用）
// ======================================================================
void showWifiQRScreen() {
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.setRotation(1);

    int dw = M5.Display.width();

    char wifiQR[128];
    snprintf(wifiQR, sizeof(wifiQR),
             "WIFI:T:WPA;S:%s;P:%s;;",
             AP_SSID, AP_PASSWORD);

    int qrSize = 180;
    int qrX = (dw - qrSize) / 2;
    int qrY = 10;

    M5.Display.qrcode(wifiQR, qrX, qrY, qrSize);

    M5.Display.setTextSize(1);
    M5.Display.setCursor(8, qrY + qrSize + 4);
    M5.Display.printf("SSID: %s\n", AP_SSID);
    M5.Display.printf("PASS: %s\n\n", AP_PASSWORD);
    M5.Display.println("B: Web用QRに切替");
    M5.Display.println("C: Avatar mode start");
}

// ======================================================================
//  QR画面（URL用）
// ======================================================================
void showUrlQRScreen() {
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.setRotation(1);

    int dw = M5.Display.width();

    const char* urlQR = "http://192.168.4.1/";

    int qrSize = 180;
    int qrX = (dw - qrSize) / 2;
    int qrY = 10;

    M5.Display.qrcode(urlQR, qrX, qrY, qrSize);

    M5.Display.setTextSize(1);
    M5.Display.setCursor(8, qrY + qrSize + 4);
    M5.Display.println("[Webコンソール用]");
    M5.Display.println("ブラウザで自動で");
    M5.Display.println("http://192.168.4.1/");
    M5.Display.println("を開きます。");
    M5.Display.println();
    M5.Display.println("B: Wi-Fi用QRに戻る");
    M5.Display.println("C: Avatar mode start");
}

// ================================================================
//  10. モード切替 & ライフサイクル（setup / loop）
// ================================================================

// ======================================================================
//  Avatarモードへの切り替え
// ======================================================================
void enterAvatarMode() {
    if (g_bootPhase == BootPhase::Avatar) return;

    M5.Display.fillScreen(BLACK);

    avatar.init();
    avatar.setExpression(Expression::Neutral);
    updateSpeech();

    initServo();
    updateLedsForTemp();
    startMQTTBroker();

    g_bootPhase = BootPhase::Avatar;

    Serial.println("[BOOT] Enter Avatar mode");
}

// ======================================================================
//  setup()
// ======================================================================
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    Serial.begin(115200);
    delay(200);

    // スピーカーの音量（0〜255）
    M5.Speaker.setVolume(64);

    randomSeed(esp_random());

    M5.Display.setRotation(1);
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(0, 0);
    M5.Display.println("Core2 Env Demo");
    M5.Display.setTextSize(1);
    M5.Display.println();
    M5.Display.println("Step1: FS init...");

    // Step1: LittleFS
    if (!LittleFS.begin(true)) {
        showFatalAndWait("LittleFS init failed");
    }

    // Step2: 設定・ログ読み込み
    M5.Display.println("Step2: load config/logs...");
    if (!loadOffsetFromFS()) {
        showWarning("No config, use offset=0.0");
    }
    if (!loadLogsFromFS()) {
        showWarning("No logs found");
    }

    // Step3: SoftAP
    M5.Display.println("Step3: start SoftAP...");
    if (!startSoftAP()) {
        showFatalAndWait("SoftAP start failed");
    }

    // Step4: HTTP server
    M5.Display.println("Step4: start HTTP...");
    server.on("/",        HTTP_GET, handleRoot);
    server.on("/offset",  HTTP_GET, handleOffset);
    server.on("/delete",  HTTP_GET, handleDelete);
    server.on("/clear",   HTTP_GET, handleClear);
    server.on("/settime", HTTP_GET, handleSetTime);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("[HTTP] Web console started on http://192.168.4.1/");

    // Step5: LED 初期化
    M5.Display.println("Step5: init LEDs...");
    initLeds();

    M5.Display.println();
    M5.Display.println("OK. Ready.");
    delay(700);

    g_bootPhase = BootPhase::QR;
    g_qrPage    = QRSubPage::Wifi;
    showWifiQRScreen();
}

// ======================================================================
//  loop()
// ======================================================================
void loop() {
    M5.update();
    server.handleClient();

    // QRモード
    if (g_bootPhase == BootPhase::QR) {
        if (M5.BtnB.wasPressed()) {
            playClickSound();
            if (g_qrPage == QRSubPage::Wifi) {
                g_qrPage = QRSubPage::Url;
                showUrlQRScreen();
            } else {
                g_qrPage = QRSubPage::Wifi;
                showWifiQRScreen();
            }
        }

        if (M5.BtnC.wasPressed()) {
            playClickSound();
            enterAvatarMode();
        }

        delay(10);
        return;
    }

    // Avatarモード
    if (M5.BtnA.wasPressed()) {
        playClickSound();
        g_showSpeech = !g_showSpeech;
        updateSpeech();
    }

    if (M5.BtnB.wasPressed()) {
        playClickSound();
        if (g_env.valid) {
            addLogEntry(g_env);
            updateSpeech();
        }
    }

    if (M5.BtnC.wasPressed()) {
        // 未使用（音だけ鳴らす等に使っても良い）
        playClickSound();
    }

    mqtt.loop();
    updateServoIdle();

    // ★ このタイミングでだけ「ぴひぃ〜」を実行
    if (g_requestScream) {
        playScreamSound();
        g_requestScream = false;
    }

    delay(10);

    mqtt.loop();
    updateServoIdle();

    delay(10);
}
