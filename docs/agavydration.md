# Agavydration 連携

M5Stack Core2 + Mini Scales Unit で植物の重量を計測し、[Agavydration](https://agavydration.pages.dev) のデバイス API に送る機能です。

実装: `src/agavydration.cpp`, `src/agav_thumb.cpp`, `src/agav_ui.cpp`

## 前提

- Agavydration 側で **device token** が発行済みであること
- 本番 URL (`https://agavydration.pages.dev` など) では **Cloudflare Access Service Token** が必要
- Mini Scales Unit を PORT-A (SDA=32, SCL=33, I2C `0x26`) に接続

## secrets.h

```c
#define AGAV_API_URL "https://agavydration.pages.dev"
#define AGAV_DEVICE_TOKEN "agav_..."
#define AGAV_CF_ACCESS_CLIENT_ID "....access"
#define AGAV_CF_ACCESS_CLIENT_SECRET "..."
```

`AGAV_API_URL` が空のときは Agavydration 連携は無効 (重量モードのみ動作)。

## 使用する API

| メソッド | パス | 用途 |
|---------|------|------|
| GET | `/api/device/plants` | 植物一覧 (起動時プリロード、C ボタンで再取得) |
| POST | `/api/device/readings` | 計量送信 `{"plant_id","weight"}` |
| GET | `/api/device/photos/*` | 選択 UI 用サムネイル (96px) |

認証:

- `Authorization: Bearer <AGAV_DEVICE_TOKEN>`
- HTTPS 時: `CF-Access-Client-Id` / `CF-Access-Client-Secret`

## 操作フロー

### 通常 (時計画面)

- **画面タップ**: 重量モードへ
- **ボタン C**: デジタル / アナログ時計の切替
- 起動後 WiFi 接続が成功していれば、バックグラウンドで植物一覧をプリロード

### 重量モード

1. スケールに植物を載せる
2. 表示重量が **2 秒間 ±0.5g 以内**で安定し、**10g 超**なら株選択画面へ
3. タッチ **左 / 右**で植物を切替 (サムネイル表示)
4. **ボタン B (短押し)**: 送信確定
5. 送信完了画面のあと、空載に戻ると計量待ちへ

その他:

| 操作 | 動作 |
|------|------|
| ボタン A | 株選択中はキャンセル、それ以外は重量モード終了 |
| ボタン B (短押し) | タレ (ゼロ調整) |
| ボタン B (長押し 0.8s) | キャリブレーション画面 |
| ボタン C | 植物一覧の再取得 |
| 60 秒アイドル | 重量モード自動終了 (載荷・選択・送信中は延長) |

## 安定検知の定数

`include/agavydration.h`:

- `AGAV_STABLE_MS` = 2000
- `AGAV_STABLE_BAND_G` = 0.5
- `AGAV_MIN_SEND_G` = 10

## バックグラウンド処理

HTTP は **core0** の FreeRTOS タスクで実行し、UI (core1) をブロックしません。

| タスク | 種別 | 内容 |
|--------|------|------|
| `agav-plants` | ワンショット | 植物一覧 GET |
| `agav-send` | ワンショット | 計量 POST |
| `thumb-download` | ワンショット | サムネイル GET |

複数タスクの HTTP は `AgavNetworkGuard` (mutex) で直列化します。

## トラブルシュート

| 症状 | 確認 |
|------|------|
| `AGAV_API_URL unset` | `secrets.h` の URL |
| `WiFi offline` | 起動時 WiFi / ルーター |
| `Network busy` | 別 HTTP タスク実行中。数秒後に再試行 |
| `POST 401/403` | device token または CF Access ヘッダ |
| スケール未検出 | 配線、I2C アドレス `0x26`、PORT-A |

## 関連

- TLS: Cloudflare Pages 用 CA は `src/agav_tls.cpp` の `AGAV_ROOT_CA`
