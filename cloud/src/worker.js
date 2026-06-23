// m5deck-alert : Datadog のアラートを受け、M5Stack がポーリングで取りに来る中継。
// 複数アラートを ID($ALERT_ID) 別に保持する。
//
// エンドポイント:
//   POST /alert    (Datadog -> Worker)  共有シークレット DD_SECRET で認証。{id,monitor,priority,message,link,ts}
//   POST /resolve  (Datadog -> Worker)  同上。{id} でその ID だけ解除（id 無しは全消し）
//   GET  /status   (M5 -> Worker)       デバイストークン DEVICE_TOKEN で認証
//   POST /ack      (M5 -> Worker)       同上。{seq}
//
// 認証は Authorization: Bearer <token> ヘッダ。/alert /resolve は ?k=<DD_SECRET> でも可。

const KEY = "alerts";

function json(obj, status = 200) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json; charset=utf-8" },
  });
}

function bearer(req) {
  const h = req.headers.get("authorization") || "";
  const m = h.match(/^Bearer\s+(.+)$/i);
  return m ? m[1] : null;
}

async function load(env) {
  const raw = await env.ALERTS.get(KEY);
  if (raw) return JSON.parse(raw);
  return { seq: 0, ackSeq: 0, items: {} };
}

async function save(env, s) {
  await env.ALERTS.put(KEY, JSON.stringify(s));
}

// デバイス向けの表示用ビュー（古い順に並べた配列＋件数）
function view(s) {
  const ids = Object.keys(s.items);
  ids.sort((a, b) => String(s.items[a].ts || "").localeCompare(String(s.items[b].ts || "")));
  const items = ids.map((id) => ({ id, ...s.items[id] }));
  return {
    state: items.length > 0 ? "ALERT" : "OK",
    count: items.length,
    seq: s.seq,
    ackSeq: s.ackSeq,
    items,
  };
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const path = url.pathname;
    const method = req.method;

    // ---- Datadog -> Worker -------------------------------------------------
    // /event は単一エンドポイント。ペイロードの transition/type/status から
    // 発報(alert)か復旧(resolve)かを Worker 側で判定する（Webhook 1 本で運用可）。
    // /alert /resolve は手動テスト用に残す。
    if (method === "POST" && (path === "/alert" || path === "/resolve" || path === "/event")) {
      const key = bearer(req) || url.searchParams.get("k");
      if (!env.DD_SECRET || key !== env.DD_SECRET) {
        return json({ error: "unauthorized" }, 401);
      }
      let body = {};
      try { body = await req.json(); } catch (_) {}

      const s = await load(env);
      const id = (String(body.id || "").slice(0, 80)) || "default";

      // アクション判定
      let action;
      if (path === "/alert") action = "alert";
      else if (path === "/resolve") action = "resolve";
      else {
        const t = (String(body.transition || "") + " " +
                   String(body.type || "") + " " +
                   String(body.status || "")).toLowerCase();
        // "recovered" / "recovered from no data" などは復旧扱い
        action = /recover/.test(t) ? "resolve" : "alert";
      }

      if (action === "alert") {
        // QR はモニターページ(/monitors/<id>)に向ける。これは Datadog アプリが
        // Universal Link として開くため、イベントリンク(/event/...)よりアプリ起動が安定。
        // org ドメインは $LINK のホストから取得（リージョン差を自動吸収）。
        const rawLink = String(body.link || "");
        let host = "app.datadoghq.com";
        try { if (rawLink) host = new URL(rawLink).host; } catch (_) {}
        const monUrl = (id && id !== "default")
          ? `https://${host}/monitors/${id}`
          : rawLink;
        s.items[id] = {
          monitor: String(body.monitor || body.title || "Datadog Alert").slice(0, 120),
          priority: String(body.priority || ""),
          message: String(body.message || "").slice(0, 240),
          link: monUrl.slice(0, 300),
          ts: String(body.ts || new Date().toISOString()),
        };
        s.seq = (s.seq || 0) + 1;  // 新規発報のたびに +1 → デバイスがサイレン再鳴動
      } else {
        if (body.id != null) delete s.items[id];
        else s.items = {};  // id 指定なしは全消し（手動リセット用）
      }
      await save(env, s);
      return json({ ok: true, action, ...view(s) });
    }

    // ---- M5 -> Worker ------------------------------------------------------
    if (method === "GET" && path === "/status") {
      if (!env.DEVICE_TOKEN || bearer(req) !== env.DEVICE_TOKEN) {
        return json({ error: "unauthorized" }, 401);
      }
      return json(view(await load(env)));
    }

    if (method === "POST" && path === "/ack") {
      if (!env.DEVICE_TOKEN || bearer(req) !== env.DEVICE_TOKEN) {
        return json({ error: "unauthorized" }, 401);
      }
      let body = {};
      try { body = await req.json(); } catch (_) {}
      const s = await load(env);
      s.ackSeq = Number(body.seq != null ? body.seq : (s.seq || 0));
      await save(env, s);
      return json({ ok: true, ackSeq: s.ackSeq });
    }

    if (path === "/") return json({ ok: true, service: "m5deck-alert" });
    return json({ error: "not found" }, 404);
  },
};
