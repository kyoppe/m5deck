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
const TTL_MS = 2 * 60 * 60 * 1000;  // アラートの寿命（2時間）。復旧を取り逃しても自動失効。

// 受信から TTL_MS を超えたアイテムを取り除く（s を破壊的に変更）
function expire(s) {
  const now = Date.now();
  for (const id of Object.keys(s.items)) {
    const rts = Number(s.items[id].rts || 0);
    if (now - rts > TTL_MS) delete s.items[id];
  }
  return s;
}

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
  const base = { seq: 0, ackSeq: 0, items: {}, remSeq: 0, remAckSeq: 0, reminders: [], notified: {} };
  if (raw) return Object.assign(base, JSON.parse(raw));
  return base;
}

const REMINDER_TTL_MS = 30 * 60 * 1000;  // カレンダー通知の表示寿命（30分）

async function save(env, s) {
  await env.ALERTS.put(KEY, JSON.stringify(s));
}

// デバイス向けの表示用ビュー（古い順に並べた配列＋件数）。
// TTL 失効はここでメモリ上だけ反映する（GET では KV を書かない）。
function view(s) {
  expire(s);
  const ids = Object.keys(s.items);
  ids.sort((a, b) => String(s.items[a].ts || "").localeCompare(String(s.items[b].ts || "")));
  const items = ids.map((id) => ({ id, ...s.items[id] }));
  // カレンダー通知（古いものは除外）
  const now = Date.now();
  const reminders = (s.reminders || [])
    .filter((r) => now - Number(r.rts || 0) < REMINDER_TTL_MS)
    .map((r) => ({ id: r.id, title: r.title, start: r.start, link: r.link }));
  return {
    state: items.length > 0 ? "ALERT" : "OK",
    count: items.length,
    seq: s.seq,
    ackSeq: s.ackSeq,
    items,
    remSeq: s.remSeq || 0,
    remAckSeq: s.remAckSeq || 0,
    reminders,
  };
}

// ---- Google Calendar（Cron で先読みして 5 分前通知を作る）-----------------
async function googleAccessToken(env) {
  const r = await fetch("https://oauth2.googleapis.com/token", {
    method: "POST",
    headers: { "content-type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({
      client_id: env.GCAL_CLIENT_ID,
      client_secret: env.GCAL_CLIENT_SECRET,
      refresh_token: env.GCAL_REFRESH_TOKEN,
      grant_type: "refresh_token",
    }),
  });
  if (!r.ok) return null;
  const j = await r.json();
  return j.access_token || null;
}

const LEAD_MS = 5 * 60 * 1000;  // 開始何分前に通知するか（5分）

async function checkCalendar(env) {
  if (!env.GCAL_REFRESH_TOKEN || !env.GCAL_CLIENT_ID || !env.GCAL_CLIENT_SECRET) return;
  const token = await googleAccessToken(env);
  if (!token) return;

  const now = Date.now();
  const timeMin = new Date(now).toISOString();
  const timeMax = new Date(now + LEAD_MS + 60 * 1000).toISOString();  // 少し先まで
  const cal = env.GCAL_CALENDAR_ID || "primary";
  const u = `https://www.googleapis.com/calendar/v3/calendars/${encodeURIComponent(cal)}/events`
    + `?singleEvents=true&orderBy=startTime&maxResults=10`
    + `&timeMin=${encodeURIComponent(timeMin)}&timeMax=${encodeURIComponent(timeMax)}`;
  const r = await fetch(u, { headers: { authorization: `Bearer ${token}` } });
  if (!r.ok) return;
  const data = await r.json();
  const events = data.items || [];

  const s = await load(env);
  let changed = false;
  for (const ev of events) {
    if (ev.status === "cancelled") continue;
    // 「自分が作成した」または「自分がYes（承諾）した」予定のみ
    const mine = ev.creator && ev.creator.self;
    const accepted = Array.isArray(ev.attendees)
      && ev.attendees.some((a) => a.self && a.responseStatus === "accepted");
    if (!mine && !accepted) continue;
    if (!ev.start || !ev.start.dateTime) continue;           // 終日予定は対象外
    const start = Date.parse(ev.start.dateTime);
    const lead = start - now;
    if (lead < 0 || lead > LEAD_MS) continue;                // 開始まで 0〜5 分
    const key = `${ev.id}@${start}`;
    if (s.notified[key]) continue;                           // 既に通知済み

    s.reminders.push({
      id: ev.id,
      title: String(ev.summary || "(no title)").slice(0, 120),
      start,
      link: String(ev.hangoutLink || ev.htmlLink || "").slice(0, 300),
      rts: now,
    });
    if (s.reminders.length > 10) s.reminders = s.reminders.slice(-10);
    s.remSeq = (s.remSeq || 0) + 1;
    s.notified[key] = now;
    changed = true;
  }
  // notified の掃除（2時間より古い）
  for (const k of Object.keys(s.notified)) {
    if (now - s.notified[k] > 2 * 60 * 60 * 1000) delete s.notified[k];
  }
  if (changed) await save(env, s);
}

export default {
  async scheduled(event, env, ctx) {
    ctx.waitUntil(checkCalendar(env));
  },

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
          rts: Date.now(),  // 受信時刻（TTL 用）
        };
        s.seq = (s.seq || 0) + 1;  // 新規発報のたびに +1 → デバイスがサイレン再鳴動
      } else {
        if (body.id != null) delete s.items[id];
        else s.items = {};  // id 指定なしは全消し（手動リセット用）
      }
      expire(s);  // 書き込み時に失効分を物理削除
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
      if (body.seq != null) s.ackSeq = Number(body.seq);
      if (body.remSeq != null) s.remAckSeq = Number(body.remSeq);
      if (body.seq == null && body.remSeq == null) s.ackSeq = s.seq || 0;
      await save(env, s);
      return json({ ok: true, ackSeq: s.ackSeq, remAckSeq: s.remAckSeq });
    }

    // デバイスからの手動クリア（アラート単位。id 無しは全消し）
    if (method === "POST" && path === "/clear") {
      if (!env.DEVICE_TOKEN || bearer(req) !== env.DEVICE_TOKEN) {
        return json({ error: "unauthorized" }, 401);
      }
      let body = {};
      try { body = await req.json(); } catch (_) {}
      const s = await load(env);
      if (body.id != null && String(body.id).length) delete s.items[String(body.id)];
      else s.items = {};
      expire(s);
      await save(env, s);
      return json({ ok: true, ...view(s) });
    }

    if (path === "/") return json({ ok: true, service: "m5deck-alert" });
    return json({ error: "not found" }, 404);
  },
};
