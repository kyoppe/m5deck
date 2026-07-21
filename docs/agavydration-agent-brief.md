# Agavydration Agent Brief: M5Deck Device API

M5Stack Core2 + Mini Scales Unit から計量データを送るための **デバイス向け API** を追加してください。
参考実装: `functions/lib/env-auth.ts` (`ENV_POLL_SECRET` Bearer パターン)。

## 背景

- **m5deck** (`~/repos/m5deck`): 重量計測は実装済み。Phase 1 では dev server の既存 API を使用中。
- **UX 決定** (2026-07-21):
  - 株は **毎回手動選択**
  - **載せて安定したら即 POST** (空に戻るのを待たない)
  - 本番認証は **device_tokens + Bearer**
  - m5deck と API 開発は **並行**

## Phase 2 要件 (agavydration 側)

### 1. DB: `device_tokens`

```sql
CREATE TABLE device_tokens (
  id TEXT PRIMARY KEY,
  user_id TEXT NOT NULL,
  token_hash TEXT NOT NULL UNIQUE,
  label TEXT,
  last_seen_at TEXT,
  created_at TEXT NOT NULL,
  FOREIGN KEY (user_id) REFERENCES allowed_users(user_id)
);
CREATE INDEX idx_device_tokens_hash ON device_tokens(token_hash);
```

- 平文トークンは発行時のみ表示。DB には SHA-256 ハッシュを保存。
- `user_id` は `allowed_users` に紐づく。

### 2. 認証 middleware

- ヘッダ: `Authorization: Bearer <DEVICE_TOKEN>`
- `functions/lib/device-auth.ts` (新規): トークン検証 → `context.data.userId` をセット
- 対象ルートのみ適用 (`/api/device/*`)。既存 Access JWT フローは変更しない。

### 3. API エンドポイント

#### `GET /api/device/plants`

軽量一覧。`GET /api/plants` の slim 版。

Response `200`:

```json
{
  "plants": [
    {
      "id": "uuid",
      "nickname": "Agave ovatifolia",
      "label": "A. ovatifolia 'Frosty Blue'",
      "latest_weight": 1234,
      "threshold": 1100,
      "hydration_status": "ok",
      "shelf_position_label": "A-2",
      "pot_weight_g": 450
    }
  ]
}
```

- `hydration_status`: `ok` | `soon` | `drying` | `learning` | `unmeasured` (既存 forecast と同じ)
- sparkline / photo / 大量メタデータは **含めない**

#### `POST /api/device/readings`

Body:

```json
{
  "plant_id": "uuid",
  "weight": 1234,
  "measured_at": "2026-07-21T03:00:00.000Z"
}
```

- `plant_id` 必須。`weight` は正の数 (g, 整数推奨)。`measured_at` は任意 ISO8601。
- 内部で既存 `insertWeightReading` + `refreshPlantListCache` + 閾値学習を **再利用**。
- Response: 既存 `POST /api/plants/:id/readings` と同型でよい (201)。

#### (任意 Phase 2.5) `POST /api/device/readings/batch`

オフラインキュー用。`{ "readings": [ { plant_id, weight, measured_at? }, ... ] }`

### 4. Web UI: トークン発行

- 設定画面に「デバイス」セクション
- ラベル入力 → トークン発行 → **一度だけ** 平文表示 + コピー
- 一覧: label, last_seen_at, 失効ボタン

### 5. ローカル dev

- `npm run dev -- --ip 0.0.0.0` (port 8788) では Access バイパス + 固定ユーザー `kyouhei`
- Phase 1 中は Bearer なしで既存 `/api/plants` + `/api/plants/:id/readings` が使える
- Phase 2 完了後、m5deck は `AGAV_DEVICE_TOKEN` を secrets に設定して `/api/device/*` に切替

## m5deck 側 (別リポジトリ、参考)

Phase 1 実装済み:

- `GET {AGAV_API_URL}/api/plants` で一覧取得
- `POST {AGAV_API_URL}/api/plants/{id}/readings` に `{ "weight": N }`
- 載荷安定 2秒 (±0.5g) で自動 POST、重複送信防止
- タッチ左右で株選択、C で一覧再取得

Phase 2 切替時の変更 (m5deck):

- URL パスを `/api/device/plants`, `/api/device/readings` に変更
- `Authorization: Bearer` ヘッダ追加

## テスト

1. `wrangler pages dev` で device API を起動
2. curl:

```bash
curl -H "Authorization: Bearer <token>" http://localhost:8788/api/device/plants
curl -X POST -H "Authorization: Bearer <token>" -H "Content-Type: application/json" \
  -d '{"plant_id":"<id>","weight":1200}' http://localhost:8788/api/device/readings
```

3. m5deck から LAN 経由で POST → D1 `weight_readings` に行が増えること

## 完了条件

- [ ] migration + `device_tokens` テーブル
- [ ] Bearer auth middleware
- [ ] `GET /api/device/plants`
- [ ] `POST /api/device/readings`
- [ ] トークン発行 UI
- [ ] 既存テスト + device auth のユニットテスト最低 1 本
