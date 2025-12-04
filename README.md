# Stack-chan Environment Monitor System

M5Stackデバイスを使用した、完全スタンドアロン・オフラインのスマート環境モニターシステムです。

このプロジェクトは、**M5Stack Core2 for AWS** を「ｽﾀｯｸﾁｬﾝ」ロボット（Wi-Fi AP + MQTTブローカー + Webサーバー）として機能させ、**M5StickC Plus2** センサーノードから環境データを受信し、温度に応じて表情やLEDの色を変化させます。

## 🌟 特徴

*   **完全オフライン & スタンドアロン**: 外部Wi-Fiルーターやインターネット接続は不要です。Core2が独自のローカルネットワークを作成します。
*   **セントラルハブ (Core2)**:
    *   **SoftAP**: Wi-Fiアクセスポイント (`Core2EnvAP`) を作成します。
    *   **MQTTブローカー**: 軽量MQTTブローカー (PicoMQTT) を実行し、センサーデータを受信します。
    *   **Webコンソール**: ログの閲覧、温度オフセットの調整、**RTC時刻設定**を行うためのローカルWebページをホストします。
    *   **スタックチャン アバター**: 温度に応じて反応するアニメーション顔を表示します (悲しい/普通/楽しい/疑い/怒り)。
    *   **インタラクティブな反応**:
        *   **表情変化**: 温度変化で表情が変わる際、弱々しい電子音（鳴き声）で知らせます。
        *   **LEDフィードバック**: 温度ゾーンに応じて本体と猫耳LEDの色が変化します。
    *   **モーション**: アイドル時のサーボ動作 (揺れ/傾き)。
*   **センサーノード (StickC Plus2)**:
    *   **ENV HAT III**: 温度、湿度、気圧を読み取ります（高度も計算・表示）。
    *   **自動接続**: Core2のWi-FiとMQTTブローカーに自動的に接続します。
    *   **リアルタイム送信**: 2秒ごとに環境データを送信します。

## 🛠 必要なハードウェア

### セントラルハブ (ロボット)
*   **M5Stack Core2 for AWS** (または通常のCore2)
*   **スタックチャン ケース & サーボ**:
    *   サーボ X (パン/左右): GPIO 33
    *   サーボ Y (チルト/上下): GPIO 32
*   **猫耳 (Nekomimi) LED** (オプション):
    *   GPIO 26 (Port B) に接続された18個のNeoPixel

### センサーノード
*   **M5StickC Plus2**
*   **M5Stack ENV HAT III** (SHT30 + QMP6988)

## 🚀 インストール & セットアップ

### 1. ファームウェアのビルドと書き込み

このプロジェクトは **PlatformIO** を使用しています。

#### Core2 (ハブ)
1.  `core2-stackchan-env` ディレクトリを開きます。
2.  M5Stack Core2を接続します。
3.  `Upload` タスクを実行します。

#### StickC Plus2 (センサー)
1.  `stickp2-env-sensor` ディレクトリを開きます。
2.  M5StickC Plus2を接続します。
3.  `Upload` タスクを実行します。

### 2. 操作方法

#### Core2 (ロボット側)

**起動時 (QRモード)**:
*   **画面表示**: Wi-Fi接続用QRコードが表示されます。
*   **ボタンB**: Wi-Fi用QR ⇔ Webコンソール用QR (`http://192.168.4.1/`) を切り替えます。
*   **ボタンC**: **アバターモード** を開始します（運用開始）。

**アバターモード**:
*   **ボタンA**: 吹き出し（温度・湿度表示）の ON/OFF 切り替え。
*   **ボタンB**: 現在のセンサー値をログに手動記録。
*   **Webコンソール**: スマホ等から `http://192.168.4.1/` にアクセスして操作します。

#### StickC Plus2 (センサー側)
![画像1](https://github.com/user-attachments/assets/ee56a2c9-bca6-40ac-bb19-e7e98ebcf85d)
1.  電源を入れると自動的に `Core2EnvAP` に接続し、計測を開始します。
2.  画面には 温度・湿度・気圧・**高度** が表示されます。

### 3. 動作仕様 (LED & リアクション)

温度に応じてスタックチャンの機嫌（表情と色）が変わります。

<img width="543" height="537" alt="スクリーンショット 2025-12-04 142142" src="https://github.com/user-attachments/assets/74985ce6-48f1-4cab-b77e-87da09cd212e" />
<img width="425" height="487" alt="スクリーンショット 2025-12-04 142627" src="https://github.com/user-attachments/assets/370f5642-919f-4717-a7dd-0fa397e9dd13" />

| 温度範囲 | 表情 (Expression) | LED色 | 状態 |
| :--- | :--- | :--- | :--- |
| **< 18.0℃** | Sad (悲しい) | **青** | 寒い |
| **< 22.0℃** | Neutral (普通) | **水色** | 肌寒い |
| **<= 26.0℃** | Happy (楽しい) | **消灯** | 快適 |
| **<= 30.0℃** | Doubt (疑い) | **ピンク** | 暑いかも |
| **> 30.0℃** | Angry (怒り) | **赤** | 暑い！ |

*   **鳴き声**: 表情が変化した際や、LEDが消灯から点灯に変わった際に、か細い電子音で鳴きます。

## 📡 技術詳細

### ネットワーク設定
*   **SSID**: `Core2EnvAP`
*   **パスワード**: `m5password`
*   **IPアドレス**: `192.168.4.1` (ゲートウェイ)

### MQTTプロトコル
*   **ブローカー**: `192.168.4.1` (ポート 1883)
*   **トピック**: `home/env/stackchan1`
*   **ペイロード形式**: CSV文字列
    ```csv
    <温度>,<湿度>,<気圧>
    ```
    *例:* `25.4,45.2,1013.2`

### Webコンソール (`http://192.168.4.1/`)
<img width="1080" height="2424" alt="392" src="https://github.com/user-attachments/assets/4c9f57bf-dae2-44bf-853b-f04cbcbd6ad5" />
*   **Current**: 現在のセンサー値確認。
*   **RTC Time**: Core2内部時計の確認と設定（スマホの時刻と同期可能）。
*   **Offset**: 温度読み取り値の校正（±0.5℃単位）。
*   **Logs**: 内部フラッシュメモリに保存された履歴データの閲覧・削除。

## 📂 プロジェクト構成

```
.
├── core2-stackchan-env/      # ハブ用ファームウェア (Core2)
│   ├── src/main.cpp          # メインロジック (SoftAP, MQTT Broker, Avatar, WebServer)
│   └── platformio.ini        # 依存関係: M5Unified, Avatar, PicoMQTT など
│
└── stickp2-env-sensor/       # センサー用ファームウェア (StickC Plus2)
    ├── src/main.cpp          # メインロジック (センサー読み取り, MQTT送信)
    └── platformio.ini        # 依存関係: M5Unified, M5UnitUnified, PubSubClient
```

## データ構造図
<img width="1379" height="1306" alt="スクリーンショット 2025-12-04 150308" src="https://github.com/user-attachments/assets/af048a97-2338-48a8-aa7d-224ee1b0be1a" />


## データ処理シーケンス
<img width="1290" height="1028" alt="スクリーンショット 2025-12-04 150402" src="https://github.com/user-attachments/assets/04e2eef5-2c5e-45e0-ad81-4cc3397e547c" />


## 📚 使用ライブラリ

*   [M5Unified](https://github.com/m5stack/M5Unified)
*   [M5Stack-Avatar](https://github.com/meganetaaan/m5stack-avatar)
*   [PicoMQTT](https://github.com/hsaturn/TinyMqtt) (ブローカー)
*   [PubSubClient](https://github.com/knolleary/pubsubclient) (クライアント)
*   [M5UnitUnified](https://github.com/m5stack/M5UnitUnified) (センサー)
*   [ESP32Servo](https://github.com/madhephaestus/ESP32Servo)
*   [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel)


## ｽﾀｯｸﾁｬﾝについて
ｽﾀｯｸﾁｬﾝはししかわさんが公開しているオープンソースのプロジェクトです。
https://github.com/stack-chan/stack-chan


## 📝 ライセンス

MIT License
