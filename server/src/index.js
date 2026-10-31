// WaitWhat deck sharing: a single Cloudflare Worker + KV.
//
//   POST /d          body = deck JSON, header X-Nonce = proof of work
//                    -> { "code": "wk7f2qxa" }
//   GET  /d/<code>   -> the deck JSON (immutable, edge cached)
//
// Anti-abuse, no accounts and no server state beyond the decks themselves:
//   - proof of work: sha256(hex(sha256(body)) + ":" + nonce) must start with
//     POW_BITS zero bits (~1s for the app, expensive to do in bulk)
//   - per-IP rate limit (Workers rate limiting binding)
//   - small size cap, strict schema, decks are re-serialized so only
//     name/q/a/note text is ever stored
//   - content addressed: re-uploading the same deck costs no KV write

const POW_BITS = 20;
const MAX_BODY = 64 * 1024;
const LIMITS = { name: 80, cards: 1000, q: 400, a: 200, answers: 8, note: 600 };
const CODE_RE = /^[a-z2-7]{8}$/;

export default {
  async fetch(req, env, ctx) {
    const url = new URL(req.url);
    try {
      if (req.method === "POST" && url.pathname === "/d") return await upload(req, env);
      if (req.method === "GET" && url.pathname.startsWith("/d/")) return await download(req, env, ctx, url);
      if (req.method === "GET" && url.pathname === "/") {
        return new Response("WaitWhat deck server. Import a code from the app.\n");
      }
      return json({ error: "not found" }, 404);
    } catch (e) {
      return json({ error: "server error" }, 500);
    }
  },
};

async function upload(req, env) {
  const ip = req.headers.get("CF-Connecting-IP") || "local";
  if (env.UPLOAD_LIMIT) {
    const { success } = await env.UPLOAD_LIMIT.limit({ key: ip });
    if (!success) return json({ error: "Too many uploads, try again in a minute." }, 429);
  }

  const len = Number(req.headers.get("Content-Length") || 0);
  if (len > MAX_BODY) return json({ error: "Deck is too big (64 KB max)." }, 413);
  const raw = new Uint8Array(await req.arrayBuffer());
  if (raw.length > MAX_BODY) return json({ error: "Deck is too big (64 KB max)." }, 413);

  const nonce = req.headers.get("X-Nonce") || "";
  if (!/^\d{1,16}$/.test(nonce)) return json({ error: "Missing proof of work." }, 400);
  const bodyHash = hex(await sha256(raw));
  const work = await sha256(new TextEncoder().encode(bodyHash + ":" + nonce));
  if (leadingZeroBits(work) < POW_BITS) return json({ error: "Invalid proof of work." }, 400);

  let deck;
  try {
    deck = clean(JSON.parse(new TextDecoder().decode(raw)));
  } catch (e) {
    return json({ error: e.message || "Invalid deck." }, 400);
  }

  const text = JSON.stringify(deck);
  const code = base32(await sha256(new TextEncoder().encode(text))).slice(0, 8);
  const key = "d:" + code;
  if ((await env.DECKS.get(key)) === null) await env.DECKS.put(key, text);
  return json({ code });
}

async function download(req, env, ctx, url) {
  const code = url.pathname.slice(3).toLowerCase();
  if (!CODE_RE.test(code)) return json({ error: "Bad code." }, 400);

  const cache = caches.default;
  const cacheKey = new Request(url.origin + "/d/" + code);
  const hit = await cache.match(cacheKey);
  if (hit) return hit;

  const text = await env.DECKS.get("d:" + code);
  if (text === null) return json({ error: "No deck with that code." }, 404);
  const res = new Response(text, {
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "public, max-age=31536000, immutable",
    },
  });
  ctx.waitUntil(cache.put(cacheKey, res.clone()));
  return res;
}

// Keep only what the app needs, trimmed and size-checked.
function clean(d) {
  if (!d || typeof d !== "object" || Array.isArray(d)) throw new Error("Deck must be an object.");
  const name = str(d.name, LIMITS.name, "name");
  if (!Array.isArray(d.cards) || d.cards.length === 0) throw new Error("Deck has no cards.");
  if (d.cards.length > LIMITS.cards) throw new Error(`Too many cards (${LIMITS.cards} max).`);
  const cards = d.cards.map((c, i) => {
    if (!c || typeof c !== "object") throw new Error(`Card ${i + 1} is not an object.`);
    const q = str(c.q, LIMITS.q, `card ${i + 1} question`);
    let a = Array.isArray(c.a) ? c.a : [c.a];
    if (a.length === 0 || a.length > LIMITS.answers) throw new Error(`Card ${i + 1}: 1-${LIMITS.answers} answers.`);
    a = a.map((x) => str(x, LIMITS.a, `card ${i + 1} answer`));
    const out = { q, a: a.length === 1 ? a[0] : a };
    if (c.note != null && c.note !== "") out.note = str(c.note, LIMITS.note, `card ${i + 1} note`);
    return out;
  });
  return { name, cards };
}

function str(v, max, what) {
  if (typeof v !== "string") throw new Error(`Missing ${what}.`);
  v = v.replace(/[\u0000-\u001f\u007f]/g, " ").trim();
  if (!v) throw new Error(`Empty ${what}.`);
  if (v.length > max) throw new Error(`${what} is too long (${max} max).`);
  return v;
}

async function sha256(bytes) {
  return new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
}

function hex(b) {
  return [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
}

function leadingZeroBits(b) {
  let n = 0;
  for (const x of b) {
    if (x === 0) { n += 8; continue; }
    return n + Math.clz32(x) - 24;
  }
  return n;
}

function base32(b) {
  const abc = "abcdefghijklmnopqrstuvwxyz234567";
  let bits = 0, val = 0, out = "";
  for (const x of b) {
    val = (val << 8) | x;
    bits += 8;
    while (bits >= 5) { out += abc[(val >>> (bits - 5)) & 31]; bits -= 5; }
  }
  return out;
}

function json(obj, status = 200) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8" },
  });
}
