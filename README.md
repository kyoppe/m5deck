# m5deck

M5Stack Core2 を卓上の情報ダッシュボードにするプロジェクト。
複数の「パネル（デッキ）」を切り替えて、ひと目で各種情報を確認できることを目指す。

## やりたいこと（ロードマップ）

- [x] 開発環境セットアップ（PlatformIO + Arduino + M5Unified）
- [x] 動作確認用サンプル（画面表示・タッチ・バッテリー・シリアル）
- [ ] 時計パネル（NTP同期）
- [ ] SwitchBot API から室温・湿度を取得して表示
- [ ] Datadog メトリクスを Query Value 風に表示
- [ ] パネル切り替え UI（タッチ / ボタン）

## 開発環境

- ハード: M5Stack Core2
- フレームワーク: Arduino (espressif32)
- ビルド: PlatformIO Core
- ライブラリ: [M5Unified](https://github.com/m5stack/M5Unified)

## セットアップ

```bash
# PlatformIO Core（pipx 経由でインストール済み）
export PATH="$HOME/.local/bin:$PATH"

# ビルド
pio run

# 書き込み（Core2 を USB-C 接続後）
pio run -t upload

# シリアルモニタ（115200 bps）
pio device monitor
```

## ディレクトリ構成

```
m5deck/
├── platformio.ini   # ビルド・ライブラリ設定
├── src/main.cpp     # メインスケッチ
├── include/  lib/  test/
└── README.md
```
