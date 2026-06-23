// m5deck-alert : Datadog のアラートを受け、M5Stack がポーリングで取りに来る中継。
//
// エンドポイント:
//   POST /alert    (Datadog -> Worker)  共有シークレット DD_SECRET で認証
//   POST /resolve  (Datadog -> Worker)  同上
//   GET  /status   (M5 -> Worker)       デバイストークン DEVICE_TOKEN で認証
//   POST /ack      (M5 -> Worker)       同上
//
// 認証は Authorization: Bearer <token> ヘッダ。/alert /resolve は ?k=<DD_SECRET> でも可。

const STATE_KEY = "state";

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

async function getState(env) {
  const raw = await env.ALERTS.get(STATE_KEY);
  if (raw) return JSON.parse(raw);
  return {
    state: "OK", monitor: "", id: "", message: "",
    priority: "", link: "", ts: "", seq: 0, ackSeq: 0,
  };
}

async function putState(env, s) {
  await env.ALERTS.put(STATE_KEY, JSON.stringify(s));
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    const path = url.pathname;
    const method = req.method;

    // ---- Datadog -> Worker -------------------------------------------------
    if (method === "POST" && (path === "/alert" || path === "/resolve")) {
      const key = bearer(req) || url.searchParams.get("k");
      if (!env.DD_SECRET || key !== env.DD_SECRET) {
        return json({ error: "unauthorized" }, 401);
      }
      let body = {};
      try { body = await req.json(); } catch (_) { /* 本文無しでも可 */ }

      const s = await getState(env);
      if (path === "/alert") {
        s.state = "ALERT";
        s.seq = (s.seq || 0) + 1;
        s.monitor = String(body.monitor || body.title || "Datadog Alert").slice(0, 120);
        s.id = String(body.id || "");
        s.message = String(body.message || "").slice(0, 240);
        s.priority = String(body.priority || "");
        s.link = String(body.link || "").slice(0, 300);
        s.ts = String(body.ts || new Date().toISOString());
      } else {
        s.state = "OK";
        s.ts = String(body.ts || new Date().toISOString());
      }
      await putState(env, s);
      return json({ ok: true, seq: s.seq, state: s.state });
    }

    // ---- M5 -> Worker ------------------------------------------------------
    if (method === "GET" && path === "/status") {
      if (!env.DEVICE_TOKEN || bearer(req) !== env.DEVICE_TOKEN) {
        return json({ error: "unauthorized" }, 401);
      }
      return json(await getState(env));
    }

    if (method === "POST" && path === "/ack") {
      if (!env.DEVICE_TOKEN || bearer(req) !== env.DEVICE_TOKEN) {
        return json({ error: "unauthorized" }, 401);
      }
      let body = {};
      try { body = await req.json(); } catch (_) {}
      const s = await getState(env);
      s.ackSeq = Number(body.seq != null ? body.seq : (s.seq || 0));
      await putState(env, s);
      return json({ ok: true, ackSeq: s.ackSeq });
    }

    if (path === "/") return json({ ok: true, service: "m5deck-alert" });
    return json({ error: "not found" }, 404);
  },
};
