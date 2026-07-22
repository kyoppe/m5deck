# m5deck ドキュメント

ファームウェアの運用・連携向けガイドです。英語のプロジェクト概要は [../README.md](../README.md) を参照してください。

## ガイド一覧

| ドキュメント | 内容 |
|-------------|------|
| [agavydration.md](agavydration.md) | 重量計測と Agavydration への送信 |
| [datadog-metrics.md](datadog-metrics.md) | Datadog カスタムメトリクス (死活・CPU・メモリ・バッテリー) |
| [display-power.md](display-power.md) | バッテリー時の Dim / Sleep とタッチ復帰 |
| [battery-runtime-test.md](battery-runtime-test.md) | バッテリー稼働時間の Datadog 計測手順 |
| [legacy-extras.md](legacy-extras.md) | ビルドから除外中の機能 (アラートサイレン・カレンダー通知など) |

## ソースとドキュメントの対応

| モジュール | 説明 |
|-----------|------|
| `src/main.cpp` | 時計パネル、重量モード、ボタン/タッチ、レガシー機能 (`ENABLE_LEGACY_EXTRAS`) |
| `src/agavydration.cpp` | `/api/device/plants`, `/api/device/readings` |
| `src/agav_thumb.cpp` | `/api/device/photos/*` サムネイル |
| `src/agav_metrics.cpp` | Datadog Metrics API v2 へ 15 秒間隔 POST |
| `cloud/` | Datadog アラート中継 Worker ([cloud/README.md](../cloud/README.md)) |

## 設定ファイル

1. `cp include/secrets.h.example include/secrets.h`
2. WiFi / Agavydration / Datadog の値を記入
3. `pio run -t upload`

`secrets.h` は git に含めません。
