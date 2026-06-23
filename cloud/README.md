# m5deck-alert (Cloudflare Worker)

Datadog のアラートを受け取り、M5Stack Core2 がポーリングで取りに来るための中継。
M5 は家庭内 NAT の内側にいて直接到達できないため、クラウド上の小さな受け皿を経由する。

```
Datadog Monitor (Alert)
  → メッセージの @webhook-m5deck-alert / @webhook-m5deck-recovery
  → この Worker (POST /alert, /resolve)   ← DD_SECRET で認証
       KV に {state, monitor, seq, ackSeq} を保存
M5Stack → 5秒ごとに GET /status            ← DEVICE_TOKEN で認証
  → ALERT 検知でサイレン → タップで POST /ack → recovery で通常へ
```

## 必要なもの

- 無料の Cloudflare アカウント
- Node.js（`npx wrangler` を使うため）

## デプロイ手順

```bash
cd cloud

# 1) Cloudflare にログイン（ブラウザが開く）
npx wrangler login

# 2) KV 名前空間を作成し、出力された id を wrangler.toml の id= に貼る
npx wrangler kv namespace create ALERTS

# 3) シークレットを2つ設定（値は自分で決めた長いランダム文字列）
#    DD_SECRET    : Datadog からの POST を認証する共有シークレット
#    DEVICE_TOKEN : M5 からの GET/POST を認証するトークン
npx wrangler secret put DD_SECRET
npx wrangler secret put DEVICE_TOKEN

# 4) デプロイ
npx wrangler deploy
# => https://m5deck-alert.<your-subdomain>.workers.dev が払い出される
```

デプロイ後の URL と `DEVICE_TOKEN` を、ファーム側の `include/secrets.h` に設定する:

```c
#define ALERT_WORKER_URL    "https://m5deck-alert.<your-subdomain>.workers.dev"
#define ALERT_DEVICE_TOKEN  "<DEVICE_TOKEN と同じ値>"
```

## 動作確認（手元から）

```bash
# アラートを擬似発報（Datadog の代わりに手で叩く）
curl -X POST "$URL/alert" -H "Authorization: Bearer $DD_SECRET" \
  -H 'content-type: application/json' \
  -d '{"monitor":"CPU high on web-01","id":"123","priority":"P1"}'

# デバイス視点で状態を見る
curl "$URL/status" -H "Authorization: Bearer $DEVICE_TOKEN"

# ACK（音停止相当）
curl -X POST "$URL/ack" -H "Authorization: Bearer $DEVICE_TOKEN" \
  -H 'content-type: application/json' -d '{"seq":1}'

# 復旧
curl -X POST "$URL/resolve" -H "Authorization: Bearer $DD_SECRET"
```

## Datadog 側の設定

### Webhook を1本だけ作成（`/event`）

発報か復旧かは Worker がペイロードの `transition`/`type` を見て自動判定するので、
Webhook は1本で済む（条件分岐も不要）。

Integrations → Webhooks → **+ New**

- 名前: `m5deck`
- URL: `https://m5deck-alert.<subdomain>.workers.dev/event`
- 「Show optional」→ Custom Headers:
  ```json
  {"Authorization": "Bearer <DD_SECRET>"}
  ```
- Payload（"Encode as form" はオフ）:
  ```json
  {
    "id": "$ALERT_ID",
    "monitor": "$ALERT_TITLE",
    "priority": "$PRIORITY",
    "message": "$TEXT_ONLY_MSG",
    "link": "$LINK",
    "transition": "$ALERT_TRANSITION",
    "type": "$EVENT_TYPE",
    "ts": "$DATE"
  }
  ```

判定ルール: `transition`/`type` に `recover` を含めば復旧（その `id` を解除）、
それ以外（Triggered / Re-Triggered / Warn / No Data）は発報として扱う。

※ Custom Headers が使えない場合は URL を `/event?k=<DD_SECRET>` のようにクエリで渡してもよい。

### モニターのメッセージに @ ハンドラを記載

サイレンを鳴らしたいモニターの通知メッセージに以下を1行追記するだけ:

```
@webhook-m5deck
```

対象モニターが Alert/Warn になったら M5 がサイレン、Recovery で通常表示に戻る。

### QR コード（モバイル）について

`/status` が返す各アラートの `link` は `https://<org>.datadoghq.com/monitors/<$ALERT_ID>`
というモニターページURLに組み立て直している（`$ALERT_ID` はモニターID）。
これは Datadog モバイルアプリが Universal Link として開くため、`$LINK`（イベントリンク）
よりアプリ起動が安定する。org ドメインは `$LINK` のホストから取得（リージョン差を自動吸収）。

### テスト

モニター編集画面右上の **Test Notifications** → **Alert** 送信でサイレン、
**Recovered** 送信で時計に戻る（実データ不要で Webhook を叩ける）。
