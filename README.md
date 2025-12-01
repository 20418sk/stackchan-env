# Stack-chan Environment Monitor System

M5Stackデバイスを使用した、完全スタンドアロン・オフラインのスマート環境モニターシステムです。

このプロジェクトは、**M5Stack Core2 for AWS** を「スタックチャン」ロボット（Wi-Fi AP + MQTTブローカー + Webサーバー）として機能させ、**M5StickC Plus2** センサーノードから環境データを受信し、温度に応じて表情やLEDの色を変化させます。

![System Overview](https://raw.githubusercontent.com/wiki/m5stack/m5-docs/assets/img/product_pics/core/core2_aws/core2_aws_01.webp)
*(注: 実際のセットアップ写真に置き換えてください)*

## 🌟 特徴

*   **完全オフライン & スタンドアロン**: 外部Wi-Fiルーターやインターネット接続は不要です。Core2が独自のローカルネットワークを作成します。
*   **セントラルハブ (Core2)**:
    *   **SoftAP**: Wi-Fiアクセスポイント (`Core2EnvAP`) を作成します。
    *   **MQTTブローカー**: 軽量MQTTブローカー (PicoMQTT) を実行し、センサーデータを受信します。
    *   **Webコンソール**: ログの閲覧や温度オフセットの調整を行うためのローカルWebページをホストします。
    *   **スタックチャン アバター**: 温度に応じて反応するアニメーション顔を表示します (悲しい/普通/楽しい/疑い/怒り)。
    *   **モーション & ライト**: アイドル時のサーボ動作 (揺れ/傾き) とLEDフィードバック (本体 + 猫耳LED)。
*   **センサーノード (StickC Plus2)**:
    *   **ENV HAT III**: 温度、湿度、気圧を読み取ります。
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

1.  **Core2の電源を入れます**。
    *   初期化され、**QRコード**が表示されます。
    *   **Wi-Fi QR**: スキャンしてスマートフォンを `Core2EnvAP` ネットワークに接続します。
    *   **URL QR**: ボタンBを押して切り替えます。スキャンしてWebコンソール (`http://192.168.4.1/`) を開きます。
    *   **ボタンC** を押して **アバターモード** を開始します。
2.  **StickC Plus2の電源を入れます**。
    *   自動的に `Core2EnvAP` Wi-Fiに接続します。
    *   接続されると、Core2へのセンサーデータ送信を開始します。
3.  **動作確認**
    *   スタックチャンの顔が温度に応じて変化します。
    *   寒い場合 (< 18°C) はLEDが **青** に、暑い場合 (> 30°C) は **赤** に点灯します。
    *   Webコンソールからログの確認や温度オフセットの調整が可能です。

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
*   **現在データ**: センサー読み取り値のリアルタイム表示。
*   **オフセット調整**: 温度読み取り値の校正 (例: センサーが熱源に近い場合など)。
*   **ログ**: 履歴データの閲覧と削除 (内部フラッシュメモリに保存)。

## 📂 プロジェクト構成

```
.
├── core2-stackchan-env/      # ハブ用ファームウェア (Core2)
│   ├── src/main.cpp          # メインロジック (SoftAP, MQTT Broker, Avatar)
│   └── platformio.ini        # 依存関係: M5Unified, Avatar, PicoMQTT など
│
└── stickp2-env-sensor/       # センサー用ファームウェア (StickC Plus2)
    ├── src/main.cpp          # メインロジック (センサー読み取り, MQTT送信)
    └── platformio.ini        # 依存関係: M5Unified, M5UnitUnified, PubSubClient
```

## 📚 使用ライブラリ

*   [M5Unified](https://github.com/m5stack/M5Unified)
*   [M5Stack-Avatar](https://github.com/meganetaaan/m5stack-avatar)
*   [PicoMQTT](https://github.com/hsaturn/TinyMqtt) (ブローカー)
*   [PubSubClient](https://github.com/knolleary/pubsubclient) (クライアント)
*   [M5UnitUnified](https://github.com/m5stack/M5UnitUnified) (センサー)
*   [ESP32Servo](https://github.com/madhephaestus/ESP32Servo)
*   [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel)

## 📝 ライセンス

MIT License