#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PicoMQTT.h>
#include <M5Unified.h>
#include <Avatar.h>
#include <LittleFS.h>
#include <math.h>
#include <Adafruit_NeoPixel.h>  // ★ 追加：NeoPixel制御用

using namespace m5avatar;

// ===== SoftAP 設定 =====
const char* AP_SSID     = "Core2EnvAP";
const char* AP_PASSWORD = "m5password";

// ===== MQTT 設定 =====
const uint16_t MQTT_PORT  = 1883;
const char*    MQTT_TOPIC = "home/env/stackchan1";  // StickP2側と合わせる

// ===== LittleFS ファイルパス =====
const char* LOG_FILE_PATH    = "/logs.csv";
const char* CONFIG_FILE_PATH = "/config.txt";

// ===== MQTT ブローカ（PicoMQTT）=====
PicoMQTT::Server mqtt;

// ===== HTTP サーバ（Webコンソール）=====
WebServer server(80);

// ===== Avatar =====
Avatar avatar;

// ===== 起動フェーズ =====
enum class BootPhase {
    QR,       // QRコード表示モード（Avatar未初期化）
    Avatar    // Avatar＋MQTT＋ボタンUIモード
};
BootPhase g_bootPhase = BootPhase::QR;

// ===== QRサブページ =====
enum class QRSubPage {
    Wifi,
    Url
};
QRSubPage g_qrPage = QRSubPage::Wifi;

// ===== 受信した環境値 =====
struct EnvReading {
    float temperature;  // ℃（オフセット適用後）
    float humidity;     // %
    float pressure;     // hPa
    bool  valid;
};

EnvReading g_env = {NAN, NAN, NAN, false};

// ===== 温度オフセット（補正） =====
float g_tempOffset = 0.0f;

// ===== ログ管理（メモリ上） =====
struct EnvLogEntry {
    float    temperature;
    float    humidity;
    float    pressure;
    uint32_t ageSec;
};

constexpr size_t LOG_CAPACITY = 32;
EnvLogEntry g_logs[LOG_CAPACITY];
size_t      g_logCount    = 0;
size_t      g_logSelected = 0;

// ===== UI モード（Avatarモード中）=====
enum class UIMode {
    Live,     // 現在値モード
    Offset    // オフセット調整モード（Core2ボタン）
};

UIMode g_mode = UIMode::Live;

// 吹き出しON/OFF
bool g_showSpeech = true;

// ===== LED（本体＋猫耳）設定 =====
// Core2 for AWS ボトムの SK6812（10個想定）
static const int BODY_LED_PIN   = 25;   // ★環境によって違ったらここを変えてください
static const int BODY_LED_COUNT = 10;

// Nekomimi LED（NeoPixel/WS2812互換）左右9個ずつ = 18個
static const int EARS_LED_PIN   = 26;   // PortB(O) = GPIO26
static const int EARS_LED_COUNT = 18;

// Adafruit NeoPixel オブジェクト
Adafruit_NeoPixel bodyStrip(BODY_LED_COUNT, BODY_LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel earsStrip(EARS_LED_COUNT,  EARS_LED_PIN,  NEO_GRB + NEO_KHZ800);

// ==== プロトタイプ ====
void updateAvatarExpression();
void updateSpeechForMode();
bool saveOffsetToFS();
bool rewriteLogsToFS();
void startMQTTBroker();
void enterAvatarMode();
void showWifiQRScreen();
void showUrlQRScreen();
void updateLedsByTemp();   // ★ 追加：温度に応じたLED更新

// ===== エラーヘルパ：致命的エラーで止める =====
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

// ===== エラーヘルパ：警告表示（処理は継続）=====
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

// ===== LEDユーティリティ =====
void setAllLedsColor(uint8_t r, uint8_t g, uint8_t b) {
    // 本体LED
    for (int i = 0; i < BODY_LED_COUNT; ++i) {
        bodyStrip.setPixelColor(i, bodyStrip.Color(r, g, b));
    }
    bodyStrip.show();

    // 猫耳LED
    for (int i = 0; i < EARS_LED_COUNT; ++i) {
        earsStrip.setPixelColor(i, earsStrip.Color(r, g, b));
    }
    earsStrip.show();
}

void turnOffAllLeds() {
    setAllLedsColor(0, 0, 0);
}

// 温度に応じて色切り替え
//   t < 18℃  → 寒い → 青
//   t > 30℃  → 暑い → 赤
//   それ以外 → 消灯
void updateLedsByTemp() {
    if (!g_env.valid) {
        // データ無いときは消灯
        turnOffAllLeds();
        return;
    }

    float t = g_env.temperature;

    if (t < 18.0f) {
        // 寒い：青
        setAllLedsColor(0, 0, 255);
    } else if (t > 30.0f) {
        // 暑い：赤
        setAllLedsColor(255, 0, 0);
    } else {
        // それ以外はオフ
        turnOffAllLeds();
    }
}

// ---- LittleFS: オフセットの読み書き ----
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

// ---- LittleFS: ログの読み書き ----
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
        unsigned long age;
        if (sscanf(line.c_str(), "%f,%f,%f,%lu", &t, &h, &p, &age) == 4) {
            EnvLogEntry e;
            e.temperature = t;
            e.humidity    = h;
            e.pressure    = p;
            e.ageSec      = age;
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
        f.printf("%.1f,%.1f,%.1f,%lu\n",
                 e.temperature,
                 e.humidity,
                 e.pressure,
                 (unsigned long)e.ageSec);
    }
    f.close();
    return true;
}

bool appendLogToFS(const EnvLogEntry& e) {
    File f = LittleFS.open(LOG_FILE_PATH, FILE_APPEND);
    if (!f) return false;

    f.printf("%.1f,%.1f,%.1f,%lu\n",
             e.temperature,
             e.humidity,
             e.pressure,
             (unsigned long)e.ageSec);
    f.close();
    return true;
}

// ---- ログ追加 ----
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
    e.ageSec      = millis() / 1000;

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

// ---- ログ削除（1件） ----
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

// ---- 全ログ削除 ----
void clearAllLogs() {
    g_logCount    = 0;
    g_logSelected = 0;
    LittleFS.remove(LOG_FILE_PATH);
}

// ---- 表情更新 ----
void updateAvatarExpression() {
    if (!g_env.valid) {
        avatar.setExpression(Expression::Neutral);
        return;
    }

    float t = g_env.temperature;
    Expression expr = Expression::Neutral;

    if (t < 18.0f) {
        expr = Expression::Sad;
    } else if (t < 22.0f) {
        expr = Expression::Neutral;
    } else if (t <= 26.0f) {
        expr = Expression::Happy;
    } else if (t <= 30.0f) {
        expr = Expression::Doubt;
    } else {
        expr = Expression::Angry;
    }

    avatar.setExpression(expr);
}

// ---- 吹き出し更新（モード別） ----
void updateSpeechForMode() {
    if (!g_showSpeech) {
        avatar.setSpeechText("");
        return;
    }

    char buf[160];

    switch (g_mode) {
        case UIMode::Live:
            if (!g_env.valid) {
                avatar.setSpeechText("Waiting MQTT...");
            } else {
                snprintf(buf, sizeof(buf),
                         "Now T:%.1fC (off:%.1f)\nH:%.0f%% P:%.0fhPa\nLogs:%d  C:Offset",
                         g_env.temperature, g_tempOffset,
                         g_env.humidity, g_env.pressure,
                         (int)g_logCount);
                avatar.setSpeechText(buf);
            }
            break;

        case UIMode::Offset:
            snprintf(buf, sizeof(buf),
                     "Offset mode\nTemp off: %.1fC\nA:-0.5  B:+0.5\nC:Back",
                     g_tempOffset);
            avatar.setSpeechText(buf);
            break;
    }
}

// ---- SoftAP 起動 ----
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

// ---- MQTT ブローカ初期化 ----
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
            updateAvatarExpression();
            if (g_mode == UIMode::Live) {
                updateSpeechForMode();
            }

            // ★ 温度更新のたびにLEDも更新
            updateLedsByTemp();
        }
    });

    mqtt.begin();
    Serial.println("[MQTT] Broker started (PicoMQTT)");
}

// ===== HTTP ハンドラ =====

void handleRoot() {
    String html;
    html.reserve(4096);

    html += F("<!DOCTYPE html><html><head><meta charset='UTF-8'>");
    html += F("<title>Stackchan Env Console</title>");
    html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    html += F("<style>body{font-family:sans-serif;margin:8px;}table{border-collapse:collapse;width:100%;}"
              "th,td{border:1px solid #ccc;padding:4px;font-size:12px;}th{background:#eee;}"
              "a.btn{display:inline-block;margin:2px 4px;padding:4px 8px;border:1px solid #333;"
              "border-radius:4px;text-decoration:none;font-size:12px;}"
              "</style></head><body>");

    html += F("<h2>Stackchan Env Console</h2>");

    // 現在値
    html += F("<h3>Current</h3><ul>");
    if (!g_env.valid) {
        html += F("<li>Waiting MQTT...</li>");
    } else {
        html += "<li>Temperature: " + String(g_env.temperature, 1) + " &deg;C (offset "
              + String(g_tempOffset, 1) + " &deg;C)</li>";
        html += "<li>Humidity: "    + String(g_env.humidity, 0) + " %</li>";
        html += "<li>Pressure: "    + String(g_env.pressure, 1) + " hPa</li>";
    }
    html += "</ul>";

    // オフセット操作
    html += F("<h3>Offset</h3>");
    html += "<p>Temp offset: <b>" + String(g_tempOffset, 1) + " &deg;C</b></p>";
    html += F("<p>"
              "<a class='btn' href='/offset?delta=-0.5'>-0.5 C</a>"
              "<a class='btn' href='/offset?delta=0.5'>+0.5 C</a>"
              "</p>");

    // ログ一覧
    html += F("<h3>Logs</h3>");
    html += "<p>Total: " + String((int)g_logCount) + "</p>";

    html += F("<table><tr><th>#</th><th>Temp</th><th>Hum</th><th>Press</th><th>Age(s)</th><th>Action</th></tr>");
    for (size_t i = 0; i < g_logCount; ++i) {
        const auto& e = g_logs[i];
        html += "<tr>";
        html += "<td>" + String((int)i) + "</td>";
        html += "<td>" + String(e.temperature, 1) + "</td>";
        html += "<td>" + String(e.humidity, 0)    + "</td>";
        html += "<td>" + String(e.pressure, 1)    + "</td>";
        html += "<td>" + String((unsigned long)e.ageSec) + "</td>";
        html += "<td><a class='btn' href='/delete?index=" + String((int)i) + "'>Delete</a></td>";
        html += "</tr>";
    }
    html += "</table>";

    if (g_logCount > 0) {
        html += F("<p><a class='btn' href='/clear'>Clear All Logs</a></p>");
    }

    html += F("<hr><p>操作メモ：<br>"
              "- 起動直後は本体画面にQRコードが出ます。<br>"
              "- スマホでWi-Fi用QR → Web用QRの順に読むと、このページを開けます。<br>"
              "- CボタンでAvatarモードに切り替え後も、このページはそのまま使えます。</p>");

    html += F("</body></html>");

    server.send(200, "text/html", html);
}

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
        if (g_mode == UIMode::Live) {
            updateSpeechForMode();
        }
        // オフセット変化時もLED更新
        updateLedsByTemp();
    }

    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "Redirecting...");
}

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

void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

// ===== QR画面（Wi-Fi用）=====
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
    M5.Display.println("[Wi-Fi接続用]");
    M5.Display.printf("SSID: %s\n", AP_SSID);
    M5.Display.printf("PASS: %s\n\n", AP_PASSWORD);
    M5.Display.println("B: Web用QRに切替");
    M5.Display.println("C: Avatar mode start");
}

// ===== QR画面（URL用）=====
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

// ===== Avatarモードへの切り替え =====
void enterAvatarMode() {
    if (g_bootPhase == BootPhase::Avatar) return;

    M5.Display.fillScreen(BLACK);

    avatar.init();
    avatar.setExpression(Expression::Neutral);
    g_mode = UIMode::Live;
    updateSpeechForMode();  // "Waiting MQTT..." 等

    startMQTTBroker();
    g_bootPhase = BootPhase::Avatar;

    Serial.println("[BOOT] Enter Avatar mode");
}

// ===== setup =====
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    Serial.begin(115200);
    delay(200);

    M5.Display.setRotation(1);
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(0, 0);
    M5.Display.println("Core2 Env Demo");
    M5.Display.setTextSize(1);
    M5.Display.println();
    M5.Display.println("Step1: FS init...");

    // ---- Step1: LittleFS ----
    if (!LittleFS.begin(true)) {
        showFatalAndWait("LittleFS init failed");
    }

    // ---- Step2: 設定・ログ読み込み ----
    M5.Display.println("Step2: load config/logs...");
    if (!loadOffsetFromFS()) {
        showWarning("No config, use offset=0.0");
    }
    if (!loadLogsFromFS()) {
        showWarning("No logs found");
    }

    // ---- Step3: SoftAP ----
    M5.Display.println("Step3: start SoftAP...");
    if (!startSoftAP()) {
        showFatalAndWait("SoftAP start failed");
    }

    // ---- Step4: HTTP server ----
    M5.Display.println("Step4: start HTTP...");
    server.on("/",       HTTP_GET, handleRoot);
    server.on("/offset", HTTP_GET, handleOffset);
    server.on("/delete", HTTP_GET, handleDelete);
    server.on("/clear",  HTTP_GET, handleClear);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("[HTTP] Web console started on http://192.168.4.1/");

    // ---- Step5: LED 初期化 ----
    M5.Display.println("Step5: init LEDs...");
    bodyStrip.begin();
    earsStrip.begin();
    bodyStrip.setBrightness(40); // 0-255 お好みで
    earsStrip.setBrightness(40);
    turnOffAllLeds();

    // ---- 起動完了 → QRモードへ ----
    M5.Display.println();
    M5.Display.println("OK. Ready.");
    delay(700);

    g_bootPhase = BootPhase::QR;
    g_qrPage    = QRSubPage::Wifi;
    showWifiQRScreen();
}

// ===== loop =====
void loop() {
    M5.update();

    // HTTP / WiFi は常に処理
    server.handleClient();

    // 起動直後の QR モード
    if (g_bootPhase == BootPhase::QR) {
        if (M5.BtnB.wasPressed()) {
            if (g_qrPage == QRSubPage::Wifi) {
                g_qrPage = QRSubPage::Url;
                showUrlQRScreen();
            } else {
                g_qrPage = QRSubPage::Wifi;
                showWifiQRScreen();
            }
        }

        if (M5.BtnC.wasPressed()) {
            enterAvatarMode();
        }

        delay(10);
        return;
    }

    // ===== ここから Avatar モード中の処理 =====

    switch (g_mode) {
        case UIMode::Live:
            if (M5.BtnA.wasPressed()) {
                g_showSpeech = !g_showSpeech;
                updateSpeechForMode();
            }
            if (M5.BtnB.wasPressed()) {
                if (g_env.valid) {
                    addLogEntry(g_env);
                    updateSpeechForMode();
                }
            }
            if (M5.BtnC.wasPressed()) {
                g_mode = UIMode::Offset;
                updateSpeechForMode();
            }
            break;

        case UIMode::Offset:
            if (M5.BtnA.wasPressed()) {
                g_tempOffset -= 0.5f;
                saveOffsetToFS();
                if (g_env.valid) {
                    g_env.temperature -= 0.5f;
                    updateAvatarExpression();
                    updateLedsByTemp();
                }
                updateSpeechForMode();
            }
            if (M5.BtnB.wasPressed()) {
                g_tempOffset += 0.5f;
                saveOffsetToFS();
                if (g_env.valid) {
                    g_env.temperature += 0.5f;
                    updateAvatarExpression();
                    updateLedsByTemp();
                }
                updateSpeechForMode();
            }
            if (M5.BtnC.wasPressed()) {
                g_mode = UIMode::Live;
                updateSpeechForMode();
            }
            break;
    }

    // MQTT ブローカ処理
    mqtt.loop();

    delay(10);
}
