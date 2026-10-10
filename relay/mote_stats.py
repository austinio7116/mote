#!/usr/bin/env python3
"""mote_stats.py -- CueVR's online play at a glance, for one private phone app.

    python3 mote_stats.py --store /data --token-file T --cert C --key K [--port 8443] [--relay 127.0.0.1:443]

A separate program from the relay, beside it on the same box. It changes
nothing: it reads <store>/results.jsonl (the results both players' games agreed
on) and asks the relay for its public LIST, the same question the game asks.

    GET /api?t=<token>   JSON: the clubs and rooms of each CueVR version now,
                         every player's totals, the last results
    GET /?t=<token>      the same as a phone-sized page that refreshes itself

HTTPS with a self-signed certificate; the app pins it, so no domain is needed.
Without the token: 403, and a wrong token waits a second before it is told so.
"""
import argparse, hmac, html, json, os, socket, ssl, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# the CueVR versions worth showing: the game id each plays online with
VERSIONS = [("7.3.2", 0x4355456F), ("7.3", 0x4355456E), ("7.2.1", 0x4355456D)]

A = None
TOKEN = b""
LOCK = threading.Lock()
CACHE = {"res_mtime": None, "results": [], "live_at": 0.0, "live": []}


def relay_list(gid):
    host, port = A.relay.rsplit(":", 1)
    s = socket.create_connection((host, int(port)), timeout=5)
    s.sendall(f"MOTE2 LIST {gid}\n".encode())
    s.settimeout(5)
    buf = b""
    try:
        while b"END\n" not in buf:
            d = s.recv(8192)
            if not d:
                break
            buf += d
    finally:
        s.close()
    clubs, rooms, live = {}, [], []
    for l in buf.decode(errors="ignore").splitlines():
        p = l.split(" ")
        if p[0] == "CLUB" and len(p) >= 4 and "/" in p[2]:
            have, mx = p[2].split("/")
            clubs[p[1]] = {"n": p[1], "have": int(have), "max": int(mx), "tables": int(p[3]), "who": []}
        elif p[0] == "CLUBWHO" and len(p) >= 3 and p[1] in clubs:
            clubs[p[1]]["who"].append(" ".join(p[2:]))
        elif p[0] == "ROOM":
            rooms.append(" ".join(p[1:]))
        elif p[0] == "LIVE":
            live.append(" ".join(p[1:]))
    return {"clubs": list(clubs.values()), "rooms": rooms, "live": live}


def live_now():
    with LOCK:
        if time.time() - CACHE["live_at"] < 4:
            return CACHE["live"]
    out = []
    for name, gid in VERSIONS:
        try:
            out.append({"version": name, **relay_list(gid)})
        except OSError as e:
            out.append({"version": name, "error": type(e).__name__, "clubs": [], "rooms": [], "live": []})
    with LOCK:
        CACHE["live"], CACHE["live_at"] = out, time.time()
    return out


def results_now():
    path = os.path.join(A.store, "results.jsonl")
    try:
        mt = os.stat(path).st_mtime
    except OSError:
        return []
    with LOCK:
        if CACHE["res_mtime"] == mt:
            return CACHE["results"]
    rows = []
    with open(path, encoding="ascii", errors="replace") as f:
        for l in f:
            try:
                rows.append(json.loads(l))
            except ValueError:
                pass
    with LOCK:
        CACHE["res_mtime"], CACHE["results"] = mt, rows
    return rows


def players(results):
    P = {}
    for line in results:
        r = line.get("result") or {}
        if r.get("t") not in ("frame", "match"):
            continue
        names = r.get("names") or ["?", "?"]
        sides = r.get("sides") or []
        for side in (0, 1):
            nm = names[side] if side < len(names) else "?"
            p = P.setdefault(nm, {"name": nm, "frames": 0, "fwon": 0, "matches": 0, "mwon": 0, "best": 0,
                                  "att": 0, "made": 0, "games": {}, "last": "", "verified": False})
            sd = sides[side] if side < len(sides) else {}
            if r["t"] == "frame":
                p["frames"] += 1
                p["fwon"] += 1 if r.get("winner") == side else 0
                p["best"] = max(p["best"], int(sd.get("best") or 0))
                sh = sd.get("shot") or {}
                p["att"] += int(sh.get("att") or 0)
                p["made"] += int(sh.get("made") or 0)
                g = (r.get("game") or "?").strip()
                p["games"][g] = p["games"].get(g, 0) + 1
            else:
                p["matches"] += 1
                p["mwon"] += 1 if r.get("match_winner") == side else 0
            p["last"] = max(p["last"], line.get("time", ""))
            p["verified"] = p["verified"] or bool(line.get("verified"))
    return sorted(P.values(), key=lambda p: (-p["frames"], p["name"]))


def recent(results, n=30):
    out = []
    for line in reversed(results):
        r = line.get("result") or {}
        if r.get("t") != "match":
            continue
        names = r.get("names") or ["?", "?"]
        fr = r.get("frames") or [0, 0]
        out.append({"time": line.get("time", ""), "game": (r.get("game") or "").strip(),
                    "variant": (r.get("variant") or "").strip(), "names": names, "frames": fr,
                    "winner": r.get("match_winner"), "verified": bool(line.get("verified"))})
        if len(out) >= n:
            break
    return out


def snapshot():
    res = results_now()
    return {"at": time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime()) + " UTC", "live": live_now(),
            "players": players(res), "recent": recent(res), "results": len(res)}


PAGE = """<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>CueVR online</title>
<style>
:root{--bg:#101418;--fg:#e4e8ec;--dim:#7d8790;--acc:#7fd4b4;--line:#242b33;--warn:#f0a070}
body{margin:0;background:var(--bg);color:var(--fg);font:15px system-ui,sans-serif}
header{position:sticky;top:0;background:var(--bg);padding:12px 16px;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:baseline}
h1{font-size:18px;margin:0}h2{font-size:14px;color:var(--acc);margin:18px 16px 6px;text-transform:uppercase;letter-spacing:.05em}
.card{margin:6px 16px;padding:10px 12px;border:1px solid var(--line);border-radius:10px}
.row{display:flex;justify-content:space-between;gap:8px}.dim{color:var(--dim)}.acc{color:var(--acc)}.warn{color:var(--warn)}
.small{font-size:13px}.tag{font-size:11px;border:1px solid var(--acc);color:var(--acc);border-radius:6px;padding:0 5px;margin-left:6px}
</style></head><body>
<header><h1>CueVR online</h1><span id=at class="dim small">...</span></header>
<div id=main></div>
<script>
const T = new URLSearchParams(location.search).get('t') || '';
const e = s => String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
/* the server's times are UTC; shown in the phone's own time */
const lt = (s, withDay) => { const d = new Date(String(s).replace(' UTC','').replace(' ','T') + 'Z');
  if (isNaN(d)) return e(s); const hm = d.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'});
  return withDay ? d.toLocaleDateString([], {day:'numeric', month:'short'}) + ' ' + hm : hm; };
function draw(d){
  document.getElementById('at').textContent = lt(d.at, false);
  let h = '<h2>Now</h2>', any = false;
  for (const v of d.live){
    for (const c of v.clubs){ any = true;
      h += `<div class=card><div class=row><b>Club ${e(c.n)}</b><span class=dim>${e(v.version)}</span></div>
            <div class="small dim">${c.have} of ${c.max} in, ${c.tables} table${c.tables==1?'':'s'} playing</div>
            <div class=small>${c.who.map(e).join(', ')||'<span class=dim>nobody named</span>'}</div></div>`; }
    for (const m of v.live){ any = true;
      h += `<div class=card><div class=row><b>Match</b><span class=dim>${e(v.version)}</span></div><div class=small>${e(m)}</div></div>`; }
    for (const r of v.rooms){ any = true;
      h += `<div class=card><div class=row><b>Waiting</b><span class=dim>${e(v.version)}</span></div><div class=small>${e(r)}</div></div>`; }
    if (v.error) h += `<div class="card warn small">${e(v.version)}: the relay did not answer (${e(v.error)})</div>`;
  }
  if (!any) h += '<div class="card dim">Nobody online right now.</div>';
  h += `<h2>Players <span class=dim>(${d.results} results kept)</span></h2>`;
  for (const p of d.players){
    const ps = p.att ? `${Math.round(100*p.made/p.att)}% (${p.made}/${p.att})` : '-';
    const games = Object.entries(p.games).sort((a,b)=>b[1]-a[1]).slice(0,3).map(([g,n])=>`${e(g)} ${n}`).join(', ');
    h += `<div class=card><div class=row><b>${e(p.name)}${p.verified?'<span class=tag>verified</span>':''}</b><span class="dim small">${lt(p.last, true)}</span></div>
          <div class=small>Frames ${p.fwon}/${p.frames} won &middot; matches ${p.mwon}/${p.matches} won &middot; best break ${p.best}</div>
          <div class="small dim">Pot success ${ps}${games?' &middot; '+games:''}</div></div>`; }
  if (!d.players.length) h += '<div class="card dim">No results kept yet.</div>';
  h += '<h2>Last matches</h2>';
  for (const r of d.recent){
    const w = r.winner===0||r.winner===1 ? r.names[r.winner] : '';
    h += `<div class=card><div class=row><span>${e(r.names[0])} <b class=acc>${r.frames[0]}-${r.frames[1]}</b> ${e(r.names[1])}</span><span class="dim small">${lt(r.time, true)}</span></div>
          <div class="small dim">${e(r.game)}${r.variant?' &middot; '+e(r.variant):''}${w?' &middot; '+e(w)+' won':''}</div></div>`; }
  document.getElementById('main').innerHTML = h;
}
async function tick(){
  try { const r = await fetch('api?t='+encodeURIComponent(T), {cache:'no-store'});
        if (r.ok) draw(await r.json()); else document.getElementById('at').textContent = 'refused ('+r.status+')'; }
  catch (x) { document.getElementById('at').textContent = 'no connection'; }
}
tick(); setInterval(tick, 10000);
</script></body></html>"""


class H(BaseHTTPRequestHandler):
    server_version = "mote-stats"

    def _ok(self, q):
        t = (q.get("t") or [""])[0].encode()
        if TOKEN and hmac.compare_digest(t, TOKEN):
            return True
        time.sleep(1.0)          # a wrong guess waits
        return False

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path not in ("/", "/api"):
            return self._send(404, "text/plain", b"not here\n")
        if not self._ok(q):
            return self._send(403, "text/plain", b"no\n")
        if u.path == "/api":
            return self._send(200, "application/json", json.dumps(snapshot()).encode())
        return self._send(200, "text/html; charset=utf-8", PAGE.encode())

    def log_message(self, fmt, *args):
        pass                     # no addresses kept


def main():
    global A, TOKEN
    ap = argparse.ArgumentParser()
    ap.add_argument("--store", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--cert", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--relay", default="127.0.0.1:443")
    A = ap.parse_args()
    with open(A.token_file, "rb") as f:
        TOKEN = f.read().strip()
    if len(TOKEN) < 24:
        raise SystemExit("the token is too short (24 characters at least)")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(A.cert, A.key)
    srv = ThreadingHTTPServer((A.bind, A.port), H)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(f"mote-stats on {A.bind}:{A.port}, results from {A.store}, relay {A.relay}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
