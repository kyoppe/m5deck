# Datadog カスタムメトリクス

Core2 の死活とデバイスヘルスを、**15 秒間隔**で Datadog Metrics API v2 に直接 POST します。UI は触らず、core0 のバックグラウンドタスクのみで動作します。

実装: `src/agav_metrics.cpp`

## 有効化

`include/secrets.h`:

```c
#define DD_API_KEY "..."
#define DD_SITE "datadoghq.com"   // US1 以外は org のサイト名
```

`DD_API_KEY` が空のときはタスクを起動しません。

## メトリクス (現在のプレフィックス)

ビルドフラグ `AGAV_METRIC_PREFIX` で名前空間を切り替えます。本番投入前は **`test.kyouhei.iot`** (テスト用)。

| メトリクス | 型 | 値 | タグ |
|-----------|-----|-----|------|
| `{prefix}.device.running` | gauge | `1` | `device:m5deck`, `version:<git-sha>` |
| `{prefix}.cpu.user` | gauge | 0-100 (2 コア平均) | 上記 + `num_cores:2` |
| `{prefix}.memory.pct_usable` | gauge | 0-100 | `device:m5deck`, `version:<git-sha>` |
| `{prefix}.battery.pct` | gauge | 0-100 | 上記 + `charging:true` / `charging:false` |

例 (テスト中):

```
test.kyouhei.iot.device.running
test.kyouhei.iot.cpu.user
test.kyouhei.iot.memory.pct_usable
test.kyouhei.iot.battery.pct
```

### プレフィックスを本番に切り替える

`platformio.ini`:

```ini
-DAGAV_METRIC_PREFIX=\"kyouhei.iot\"
```

再ビルド・書き込み後、Datadog 上は別メトリクス名になります。モニターも合わせて更新してください。

## 送信先

```
POST https://api.{DD_SITE}/api/v2/series
Header: DD-API-KEY: <DD_API_KEY>
```

4 メトリクスを **1 リクエスト**にまとめて送ります。TLS は `api.datadoghq.com` 用 CA (`DD_API_ROOT_CA` in `src/agav_tls.cpp`)。

## CPU の算出

Linux の `system.cpu.user` と同様、**ホスト全体で 0-100%** です。

- core0 / core1 のアイドルフックで 100µs 間隔サンプリング
- 15 秒ウィンドウの平均: `cpu.user = 100 - (idle0% + idle1%) / 2`
- `num_cores:2` は `cpu.user` のみ (メタデータタグ)

## Wi-Fi と死活監視の方針

| 状況 | 動作 |
|------|------|
| Wi-Fi 未接続 | 送信スキップ、`WiFi.reconnect()` を試行 |
| NTP 未同期 | 送信スキップ (timestamp が不正になるため) |
| 送信失敗 | バッファ・遡及なし。次の 15 秒 tick で再試行 |

**意図的にバッファしません。** 届かなかった間はメトリクスが欠損し、それが死活監視として正しい挙動です。

## モニター例

```
avg(last_1m):test.kyouhei.iot.device.running{device:m5deck} < 1
```

プレフィックス変更後は `kyouhei.iot.device.running` に読み替えてください。

## バージョンタグ

`scripts/git_version.py` がビルド時に git short SHA を `M5DECK_VERSION` として注入し、全メトリクスに `version:<sha>` タグを付けます。

## シリアルログ

| ログ | 意味 |
|------|------|
| `metrics: started prefix=... version=...` | タスク起動 |
| `metrics: sent` | POST 成功 |
| `metrics: POST failed code=...` | HTTP エラー (本文も出力) |
| `metrics: WiFi offline, reconnecting` | 送信スキップ |
| `metrics: skip (clock not synced)` | NTP 待ち |
| `metrics: DD_API_KEY not set, disabled` | 無効 |

## 他機能との関係

- HTTP は `AgavNetworkGuard` 経由 (agavydration / サムネイルと共有 mutex)
- [cloud/README.md](../cloud/README.md) のアラート Worker とは **別経路** (デバイスから Metrics API 直 POST)
- 画面への Query Value 表示は未実装 (ロードマップ)

## トラブルシュート

| 症状 | 対処 |
|------|------|
| メトリクスが Explorer に出ない | 初回は数分かかることがある。`metrics: sent` をシリアルで確認 |
| TLS エラー | `DD_API_ROOT_CA` が `api.datadoghq.com` 用か確認 |
| 常に `clock not synced` | 起動時 WiFi / NTP (`syncTime` in `main.cpp`) |
| CPU が送信時だけ跳ねる | TLS POST の瞬間負荷。15 秒平均なら許容範囲 |
