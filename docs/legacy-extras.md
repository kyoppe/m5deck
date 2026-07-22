# Legacy extras (ビルド無効)

`src/main.cpp` 先頭の `ENABLE_LEGACY_EXTRAS` が **`0`** のとき、以下はコンパイル・リンクされません。デフォルトの現行ビルドは **時計 + 重量 + Agavydration + Datadog メトリクス** のみです。

```cpp
#define ENABLE_LEGACY_EXTRAS 0
```

`1` にすると再び有効になります (ファームサイズ・RAM 使用量が増えます)。

## 含まれる機能

| 機能 | 概要 | 外部依存 |
|------|------|----------|
| Datadog アラートサイレン | Monitor 発報でサイレン、タップで ACK | [cloud/](../cloud/) Worker |
| Google カレンダー通知 | 予定 5 分前に画面表示 | Worker 経由 (同一 KV) |
| ミュート UI | サイレン・通知の一時ミュート | なし |
| IMU 持ち上げ検出 | 持ち上げると「怒り」画面 + 音声 | `AngryVoice.h` |

## Datadog アラート中継

デプロイと Datadog Webhook 設定: [cloud/README.md](../cloud/README.md)

`secrets.h`:

```c
#define ALERT_WORKER_URL "https://m5deck-alert.<subdomain>.workers.dev"
#define ALERT_DEVICE_TOKEN "..."
```

M5 は 5 秒ごとに `GET /status` をポーリングします (core0 `alertTask`)。

## 再有効化の手順

1. `main.cpp` で `#define ENABLE_LEGACY_EXTRAS 1`
2. Worker をデプロイし `ALERT_*` を `secrets.h` に設定
3. `pio run -t upload`

Agavydration / Datadog メトリクスとは独立して動作します。
