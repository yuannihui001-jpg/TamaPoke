// TamaPoke strict license gateway.
// Release 2.34.0: automatic WiFi OTA check, encrypted origin, and browser grants.
// Bind a Workers KV namespace (tomagochi or LICENSES) and add FIRMWARE_KEY as a secret.

const JSON_HEADERS = {
  "content-type": "application/json; charset=utf-8",
  "cache-control": "no-store",
  "access-control-allow-origin": "*",
  "access-control-allow-headers": "authorization,content-type,x-tamapoke-device,x-tamapoke-version,x-tamapoke-release",
  "access-control-allow-methods": "GET,POST,OPTIONS",
};

const FIRMWARE_ORIGIN =
  "https://raw.githubusercontent.com/yuannihui001-jpg/TamaPoke/main/web/firmware/";
const INSTALLER_ORIGIN =
  "https://raw.githubusercontent.com/yuannihui001-jpg/TamaPoke/main/web/index.html";
const SPRITES_ORIGIN =
  "https://raw.githubusercontent.com/yuannihui001-jpg/TamaPoke/main/web/sprites.pak";
const GRANT_TTL_SECONDS = 10 * 60;
// Authorized devices keep a long-lived device-bound token so routine OTA
// updates do not ask the owner for the license again. Browser install grants
// remain short-lived and still require the author license.
const DEVICE_GRANT_TTL_SECONDS = 365 * 24 * 60 * 60;
const RELEASE_VERSION = "2.34.0";
const RELEASE_PROOFS = {
  // Keep the previous official release eligible for a one-time bootstrap so
  // v2.31 devices can update without asking for the author license again.
  "2.31.0": "TamaPoke-2.31.0-official",
  "2.32.0": "TamaPoke-2.32.0-official",
  "2.33.0": "TamaPoke-2.33.0-official",
  "2.34.0": "TamaPoke-2.34.0-official",
};

function json(value, status = 200) {
  return new Response(JSON.stringify(value), { status, headers: JSON_HEADERS });
}

function b64url(bytes) {
  let binary = "";
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function fromB64url(value) {
  const padded = value.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((value.length + 3) % 4);
  const binary = atob(padded);
  return Uint8Array.from(binary, (c) => c.charCodeAt(0));
}

async function sha256(value) {
  const bytes = new TextEncoder().encode(value);
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function licenseStore(env) {
  // Accept the variable name already configured in the dashboard, while keeping
  // LICENSES as the documented name for CLI deployments.
  return env.LICENSES || env.tomagochi;
}

async function randomToken() {
  const bytes = crypto.getRandomValues(new Uint8Array(32));
  return b64url(bytes);
}

function deviceIdFrom(request) {
  return (request.headers.get("x-tamapoke-device") || "").trim().toLowerCase();
}

function officialDevice(request) {
  const deviceId = deviceIdFrom(request);
  const version = (request.headers.get("x-tamapoke-version") || "").trim();
  const proof = (request.headers.get("x-tamapoke-release") || "").trim();
  return !!deviceId && deviceId.length <= 40 && !!RELEASE_PROOFS[version] && RELEASE_PROOFS[version] === proof;
}

function bearer(request) {
  const value = request.headers.get("authorization") || "";
  return value.startsWith("Bearer ") ? value.slice(7).trim() : "";
}

async function loadToken(env, token) {
  if (!token) return null;
  const key = "token:" + await sha256(token);
  const raw = await licenseStore(env).get(key);
  if (!raw) return null;
  try {
    const data = JSON.parse(raw);
    if (!data.exp || data.exp < Math.floor(Date.now() / 1000)) return null;
    return data;
  } catch (_) {
    return null;
  }
}

async function issueGrant(env, deviceId, kind) {
  const token = await randomToken();
  const ttl = kind === "device" ? DEVICE_GRANT_TTL_SECONDS : GRANT_TTL_SECONDS;
  const exp = Math.floor(Date.now() / 1000) + ttl;
  await licenseStore(env).put("token:" + await sha256(token), JSON.stringify({ deviceId, kind, exp }), {
    expirationTtl: ttl,
  });
  return { token, exp };
}

async function decryptFirmware(env, filename) {
  const response = await fetch(FIRMWARE_ORIGIN + filename, { cf: { cacheTtl: 0 } });
  if (!response.ok) throw new Error("encrypted firmware fetch failed: " + response.status);
  const encrypted = new Uint8Array(await response.arrayBuffer());
  if (encrypted.length < 29) throw new Error("encrypted firmware is too small");
  const iv = encrypted.slice(0, 12);
  const ciphertext = encrypted.slice(12);
  const keyBytes = fromB64url(env.FIRMWARE_KEY);
  const key = await crypto.subtle.importKey("raw", keyBytes, "AES-GCM", false, ["decrypt"]);
  return crypto.subtle.decrypt({ name: "AES-GCM", iv }, key, ciphertext);
}

async function activate(request, env) {
  let body;
  try { body = await request.json(); } catch (_) { return json({ error: "invalid_json" }, 400); }
  const license = String(body.license || "").trim();
  const deviceId = String(body.deviceId || "").trim().toLowerCase();
  if (!license || !deviceId || deviceId.length > 40) return json({ error: "license_and_device_required" }, 400);
  const licenseKey = "license:" + await sha256(license);
  const raw = await licenseStore(env).get(licenseKey);
  if (!raw) return json({ error: "license_not_found" }, 403);
  let record;
  try { record = JSON.parse(raw); } catch (_) { return json({ error: "license_invalid" }, 403); }
  if (record.revoked) return json({ error: "license_revoked" }, 403);
  if (record.deviceId && record.deviceId !== deviceId) return json({ error: "license_bound_to_other_device" }, 409);
  record.deviceId = deviceId;
  record.activatedAt = record.activatedAt || new Date().toISOString();
  await licenseStore(env).put(licenseKey, JSON.stringify(record));
  const issued = await issueGrant(env, deviceId, "device");
  return json({ token: issued.token, expiresAt: issued.exp });
}

async function deviceBootstrap(request, env) {
  const deviceId = deviceIdFrom(request);
  const version = (request.headers.get("x-tamapoke-version") || "").trim();
  const proof = (request.headers.get("x-tamapoke-release") || "").trim();
  if (!deviceId || deviceId.length > 40 || RELEASE_PROOFS[version] !== proof)
    return json({ error: "forbidden" }, 403);
  const issued = await issueGrant(env, deviceId, "device");
  return json({ token: issued.token, expiresAt: issued.exp, deviceId });
}

async function browserManifest(request, env) {
  let body;
  try { body = await request.json(); } catch (_) { return json({ error: "invalid_json" }, 400); }
  const license = String(body.license || "").trim();
  if (!license) return json({ error: "license_required" }, 400);
  const raw = await licenseStore(env).get("license:" + await sha256(license));
  if (!raw) return json({ error: "license_not_found" }, 403);
  let record;
  try { record = JSON.parse(raw); } catch (_) { return json({ error: "license_invalid" }, 403); }
  if (record.revoked) return json({ error: "license_revoked" }, 403);
  const issued = await issueGrant(env, "browser", "install");
  const base = new URL(request.url).origin;
  return json({
    version: RELEASE_VERSION,
    manifest: {
      name: "TamaPoke",
      version: RELEASE_VERSION,
      new_install_prompt_erase: true,
      builds: [{ chipFamily: "ESP32-S3", parts: [{ path: base + "/v1/install?grant=" + encodeURIComponent(issued.token), offset: 0 }] }],
    },
    expiresAt: issued.exp,
  });
}

async function firmware(request, env, grantKind) {
  const url = new URL(request.url);
  const token = bearer(request) || url.searchParams.get("grant") || "";
  const record = await loadToken(env, token);
  const binaryHeaders = {
    "content-type": "text/plain; charset=utf-8",
    "access-control-allow-origin": "*",
    "cache-control": "no-store",
  };
  const deviceId = deviceIdFrom(request);
  // Official firmware carries a release proof, so routine OTA never asks the
  // owner to enter a machine license. Browser installs still require a grant.
  const official = grantKind === "device" && officialDevice(request);
  const authorized = record && record.kind === grantKind &&
    (grantKind !== "device" || (deviceId && record.deviceId === deviceId));
  if (!official && !authorized) return new Response("Forbidden", { status: 403, headers: binaryHeaders });
  try {
    const data = await decryptFirmware(env, grantKind === "install" ? "tamapoke-2.34.0-merged.bin.enc" : "tamapoke-2.34.0-app.bin.enc");
    return new Response(data, {
      headers: {
        "content-type": "application/octet-stream",
        "content-length": String(data.byteLength),
        "cache-control": "no-store",
        // ESP Web Tools downloads this one-time URL from GitHub Pages.
        "access-control-allow-origin": "*",
        "x-tamapoke-license": "verified",
        "x-tamapoke-release": RELEASE_VERSION,
      },
    });
  } catch (error) {
    return new Response("Firmware unavailable: " + error.message, { status: 503, headers: binaryHeaders });
  }
}

async function deviceVersion(request, env) {
  const record = await loadToken(env, bearer(request));
  const deviceId = deviceIdFrom(request);
  const authorized = record && record.kind === "device" && deviceId && record.deviceId === deviceId;
  if (!officialDevice(request) && !authorized)
    return json({ error: "forbidden" }, 403);
  return json({ version: RELEASE_VERSION, deviceId, update: (request.headers.get("x-tamapoke-version") || "") !== RELEASE_VERSION });
}

export default {
  async fetch(request, env) {
    if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: JSON_HEADERS });
    const url = new URL(request.url);
    if (url.pathname === "/" && request.method === "GET") {
      const page = await fetch(INSTALLER_ORIGIN, { cf: { cacheTtl: 0 } });
      if (!page.ok) return new Response("Installer unavailable", { status: 503 });
      return new Response(await page.text(), {
        headers: {
          "content-type": "text/html; charset=utf-8",
          "cache-control": "no-store",
        },
      });
    }
    if (url.pathname === "/sprites.pak" && request.method === "GET") {
      const sprites = await fetch(SPRITES_ORIGIN, { cf: { cacheTtl: 3600 } });
      if (!sprites.ok) return new Response("Sprites unavailable", { status: 503 });
      return new Response(sprites.body, {
        headers: {
          "content-type": "application/octet-stream",
          "cache-control": "public, max-age=3600",
        },
      });
    }
    if (url.pathname === "/v1/activate" && request.method === "POST") return activate(request, env);
    if (url.pathname === "/v1/device-bootstrap" && request.method === "POST") return deviceBootstrap(request, env);
    if (url.pathname === "/v1/browser-manifest" && request.method === "POST") return browserManifest(request, env);
    if (url.pathname === "/v1/device-version" && request.method === "GET") return deviceVersion(request, env);
    if (url.pathname === "/v1/firmware" && request.method === "GET") return firmware(request, env, "device");
    if (url.pathname === "/v1/install" && request.method === "GET") return firmware(request, env, "install");
    if (url.pathname === "/health") {
      return json({ ok: true, service: "tamapoke-license", version: RELEASE_VERSION });
    }
    return json({ error: "not_found" }, 404);
  },
};
