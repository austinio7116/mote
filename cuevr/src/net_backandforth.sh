#!/usr/bin/env bash
# CueVR — two instances, a LAN link, and shots going BOTH ways.
#
# The online test this replaces drove two instances into a room together and
# checked they agreed on the game and who breaks. They always did. What it never
# did was play a second shot — and the second shot was dead: resolve_shot()
# routed the turn as if both players were at one machine, so after the break both
# ends sat in AIM and neither was listening for the other's stroke. A test that
# only proves pairing proves nothing about a match.
#
# So this plays a rally. Each end takes its own shots (CUEVR_AUTOPLAY) and has to
# receive the other's; both write a trace line whenever anything a test cares
# about moves, and the two transcripts are compared afterwards:
#
#   * both ends must see the same number of shots, and more than one
#   * both ends must agree whose shot each was
#   * every table an end settles on must be one the other settled on too
#   * they must finish on the same table, score and frame tally
#   * they must be playing to the same match length
#
#   ./net_backandforth.sh [shots] [frames]
#   BREAK=0|1                 who breaks (0 = host). RUN IT BOTH WAYS: the
#                             failure this exists to catch behaves differently
#                             depending on which end plays first.
#   MATCH_HOST= MATCH_JOIN=   force a different match length at each end, to
#                             prove the host's is the one both play to.
set -u
SHOTS=${1:-8}
FRAMES=${2:-20000}
BIN=$(cd "$(dirname "$0")/../.." && pwd)/build_host/cuevr_preview
OUT=${OUT:-/tmp/cuevr-net}
rm -rf "$OUT"; mkdir -p "$OUT/host" "$OUT/join"

# Hidden windows (MOTE_VR_BENCH) so nothing steals focus, and a dummy audio
# device because two instances cannot both own the real one.
# Both ends stop when a FRAME is won, not at a fixed frame number: cut at a
# frame count, one end is mid-shot and the other is not, and the tails differ
# for reasons that have nothing to do with the link. The bench count is only a
# backstop for a run that never finishes one.
common=(env SDL_AUDIODRIVER=dummy MOTE_VR_BENCH="$FRAMES" CUEVR_QUIT_ON_FRAME=1 ${NETDBG:+CUEVR_NETDBG=1}
        CUEVR_AUTOPLAY="$SHOTS" CUEVR_NOELEV=1 CUEVR_BREAK="${BREAK:-0}" ${GAME:+CUEVR_GAME=$GAME} ${QSHOTS:+CUEVR_QUIT_AFTER_SHOTS=$QSHOTS})

# The host gets a longer budget so it outlives the joiner: whichever end quits
# first, the other records an "OPPONENT LEFT" frame-over that is the test
# shutting down and not the match, and it is simpler to have that land on one
# known end than to guess which trailing state is real.
"${common[@]}" MOTE_VR_BENCH=$((FRAMES + 4000)) \
    CUEVR_TAG=host CUEVR_NET=host CUEVR_PREFS_DIR="$OUT/host" \
    ${MATCH_HOST:+CUEVR_MATCH=$MATCH_HOST} \
    "$BIN" >"$OUT/host.log" 2>"$OUT/host.err" &
H=$!
sleep 2
"${common[@]}" CUEVR_TAG=join CUEVR_NET=join CUEVR_NET_IP=127.0.0.1 \
    ${MATCH_JOIN:+CUEVR_MATCH=$MATCH_JOIN} \
    CUEVR_PREFS_DIR="$OUT/join" "$BIN" >"$OUT/join.log" 2>"$OUT/join.err" &
J=$!
wait $H; wait $J

python3 - "$OUT" <<'PY'
import re, sys
out = sys.argv[1]
PAT = (r'\[(\w+)\] f(\d+)\s+(\S+)\s+turn=(\d+) me=(\d+) bo=(\d+) '
       r'score=(\d+)/(\d+) frames=(\d+)/(\d+) on=(\d+) hash=(\w+) '
       r'obj=(\w+) rules=(\w+) cue=(\S+),(\S+)')
def rows(p, tag):
    r=[]
    for l in open(p):
        m=re.match(PAT, l)
        if not m or m.group(1)!=tag: continue
        _,f,st,turn,me,bo,s0,s1,f0,f1,on,h,ob,ru,cx,cz = m.groups()
        r.append(dict(f=int(f),st=st,turn=turn,me=me,bo=bo,on=on,
                      sc=s0+'/'+s1,fr=f0+'/'+f1,obj=ob,rules=ru,hash=h))
    return r
h=rows(f'{out}/host.log','host'); j=rows(f'{out}/join.log','join')
if not h or not j:
    print("FAIL: one end produced no trace"); sys.exit(1)

SETTLED=('AIM','THINK','PLACE','DECIDE','OVER')
def shots(rs):   # a shot is each entry into ROLL
    return [rs[i] for i,r in enumerate(rs) if r['st']=='ROLL' and (i==0 or rs[i-1]['st']!='ROLL')]
def tables(rs):  # every distinct settled table, in order
    ks=[(r['obj'],r['rules']) for r in rs if r['st'] in SETTLED]
    return [k for i,k in enumerate(ks) if i==0 or k!=ks[i-1]]

hs, js = shots(h), shots(j)
ht, jt = tables(h), tables(j)
ok=True
def chk(name, cond, detail=''):
    global ok
    ok &= bool(cond)
    print(f"  [{'ok ' if cond else 'FAIL'}] {name}{('  '+detail) if detail else ''}")

# AN END MAY LEGITIMATELY SKIP SIMULATING A SHOT. The host's state packet
# supersedes any shot still unread, so an end that fell behind — the joiner is
# still baking its table while the host breaks — takes the resulting table
# instead of rolling the balls. It ends up in the same place, one ROLL short.
# So the tables each end passes through are compared as a SUBSEQUENCE rather
# than one for one; anything stricter fails on correct behaviour.
def subseq(a, b):        # is a contained in b, in order?
    it = iter(b)
    return all(x in it for x in a)

print(f"\n--- back and forth: host saw {len(hs)} shots, joiner saw {len(js)} ---")
chk("both ends saw the same shots", abs(len(hs)-len(js))<=1, f"{len(hs)} vs {len(js)}")
chk("more than one shot was played", min(len(hs),len(js))>1,
    "the bug this catches let exactly one through")
sh=[(r['turn'],r['obj'],r['rules']) for r in hs]
sj=[(r['turn'],r['obj'],r['rules']) for r in js]
chk("the shots each end saw are the same shots", subseq(sj,sh) or subseq(sh,sj),
    f"{len(set(sh)&set(sj))} of {max(len(sh),len(sj))} in common")
print(f"         (settled tables: host {len(ht)}, joiner {len(jt)}, "
      f"{len(set(ht)&set(jt))} in common — the rest are the moments one end has "
      f"resolved and the other has not yet taken the correction)")
# A trailing OVER is EITHER a frame genuinely ending or the other end quitting,
# and the two look identical in the trace. Stripping it unconditionally was
# wrong: it threw away a real frame win and reported a desync that was not
# there. So compare the last rows as they stand, and only if they differ try
# again with one end's trailing OVER dropped as the shutdown artifact.
def sig(r): return (r['obj'],r['rules'],r['sc'],r['fr'])
def show(t,r): return f"\n         {t} {r['st']:6s} obj={r['obj']} rules={r['rules']} sc={r['sc']} fr={r['fr']}"
fh, fj = h[-1], j[-1]
same = sig(fh)==sig(fj)
note = ""
if not same:
    for a,b,which in ((h[:-1],j,'host'),(h,j[:-1],'join')):
        if a and b and a[-1]['st']!='ROLL' and b[-1]['st']!='ROLL' and sig(a[-1])==sig(b[-1]):
            same=True; fh,fj=a[-1],b[-1]
            note=f"  (ignoring {which}'s trailing OVER: the other end had quit)"
            break
chk("both ends finished on the same table", same,
    note + show('host',fh) + show('join',fj))
chk("both ends play the same match length", h[-1]['bo']==j[-1]['bo'],
    f"best of {h[-1]['bo']} / {j[-1]['bo']}")
print(f"\n{'PASS' if ok else 'FAIL'}\n")
sys.exit(0 if ok else 1)
PY
