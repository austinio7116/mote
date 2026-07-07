#!/usr/bin/env python3
"""Generate docs/games.json — the machine-readable gallery manifest the Studio and
on-device gallery viewers consume.

It merges two sources:
  * the curated GAMES array in docs/games.html (display name, author, tagline,
    description, multiplayer flag, screenshot count) — the human-facing metadata; and
  * each docs/games/<file>.mote binary (embedded version via version_vaddr, ABI,
    byte size, sha256) — the machine-facing "what version is this file, and did it
    download intact" metadata.

Run from the repo root:  python3 tools/gen_gallery.py
The .mote files must already be built (with MOTE_GAME_VERSION) into docs/games/.
"""
import os, re, json, struct, hashlib, sys, datetime

ROOT   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS   = os.path.join(ROOT, "docs")
HTML   = os.path.join(DOCS, "games.html")
GDIR   = os.path.join(DOCS, "games")
OUT    = os.path.join(DOCS, "games.json")
VADDR  = 0x10800000
MAGIC  = 0x45544F4D          # 'MOTE'

def parse_html_games():
    """Pull the GAMES = [ {...}, ... ] array out of docs/games.html."""
    src = open(HTML, encoding="utf-8").read()
    m = re.search(r'const\s+GAMES\s*=\s*\[(.*?)\];', src, re.S)
    if not m: sys.exit("gen_gallery: GAMES array not found in games.html")
    body = m.group(1)
    games = []
    # each top-level {...} object (descriptions have no nested braces)
    for obj in re.finditer(r'\{(.*?)\}', body, re.S):
        o = obj.group(1)
        def s(key):
            mm = re.search(r'\b%s\s*:\s*"((?:[^"\\]|\\.)*)"' % key, o)
            return json.loads('"%s"' % mm.group(1)) if mm else None
        def n(key):
            mm = re.search(r'\b%s\s*:\s*(\d+)' % key, o)
            return int(mm.group(1)) if mm else None
        gid = s("id")
        if not gid: continue
        games.append({
            "id": gid, "file": s("file"), "name": s("name"), "author": s("by"),
            "tag": s("tag"), "desc": s("desc"),
            "multiplayer": bool(n("mp")), "shots": n("n") or 3,
        })
    return games

def mote_meta(path):
    """version / abi / size / sha256 read straight from the .mote binary."""
    data = open(path, "rb").read()
    hdr = struct.unpack_from("<13I", data, 0)
    magic, abi = hdr[0], hdr[1]
    version_vaddr = hdr[12] if len(hdr) > 12 else 0
    ver = "0"
    if magic == MAGIC and abi >= 46 and version_vaddr:
        off = version_vaddr - VADDR
        if 0 <= off < len(data):
            end = data.find(b"\0", off)
            if end > off: ver = data[off:end].decode("ascii", "replace")
    return {
        "version": ver,
        "abi": abi if magic == MAGIC else 0,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }

def main():
    curated = parse_html_games()
    games = []
    for g in curated:
        f = g["file"]
        p = os.path.join(GDIR, f)
        if not os.path.isfile(p):
            print("  ! missing binary: %s (skipped)" % f); continue
        meta = mote_meta(p)
        gid = g["id"]
        icon  = "img/gallery/%s-icon.png" % gid
        thumb = "img/gallery/%s-1.png" % gid
        shots = ["img/gallery/%s-%d.png" % (gid, i) for i in range(1, g["shots"] + 1)]
        games.append({
            "id": gid,
            "name": g["name"],
            "author": g["author"],
            "version": meta["version"],
            "abi": meta["abi"],
            "size": meta["size"],
            "sha256": meta["sha256"],
            "file": "games/%s" % f,
            "icon": icon if os.path.isfile(os.path.join(DOCS, icon)) else None,
            "thumb": thumb if os.path.isfile(os.path.join(DOCS, thumb)) else None,
            "screenshots": [s for s in shots if os.path.isfile(os.path.join(DOCS, s))],
            "tag": g["tag"],
            "desc": g["desc"],
            "multiplayer": g["multiplayer"],
        })
        print("  %-16s v%-7s abi%d %7d B  %s" % (gid, meta["version"], meta["abi"], meta["size"], meta["sha256"][:12]))
    manifest = {
        "schema": 1,
        "generated": datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat(),
        "min_firmware": "1.34.0",     # informational; per-game `abi` is the real gate
        "games": games,
    }
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=1, ensure_ascii=False)
        fh.write("\n")
    print("wrote %s — %d games" % (os.path.relpath(OUT, ROOT), len(games)))

if __name__ == "__main__":
    main()
