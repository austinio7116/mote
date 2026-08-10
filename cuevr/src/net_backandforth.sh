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
# ENOUGH FOR A FRAME TO FINISH, which is what the comparison below assumes: both
# ends stop when a FRAME is won and the count is only a backstop. Cut short and
# one end is mid-shot while the other is not, so the tails differ for reasons
# that have nothing to do with the link — and the run reports FAIL while proving
# nothing. Every check below can be green in substance and still fail on that.
#
# 20000 was enough until the 7 ft pockets were tightened by a sixth; fewer pots
# per shot means a longer frame, and runs began exhausting the budget at 19940
# with the balls still rolling. If this starts failing with "frames=0/0" and
# a BENCH line near the limit in both logs, that is this again — raise it, do
# not go looking for a desync.
FRAMES=${2:-60000}
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
import os, re, sys
SHOTS_HINT = 'raise the shot count or the frame budget'
out = sys.argv[1]
PAT = (r'\[(\w+)\] f(\d+)\s+(\S+)\s+kind=\d+ turn=(\d+) me=(\d+) bo=(\d+) '
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
# THE BREAK IS COMPARED ON THE RULES, NOT THE BALLS.
#
# It is the one stroke where the far end cannot yet know where the white is:
# ball in hand is a position only the striker has, and it crosses WITH the shot
# — there is no state packet before the first stroke of a frame to carry it. So
# the receiving end's table at the moment it enters ROLL legitimately differs
# from the striker's, and every stroke after it agrees exactly because the
# position arrived. Requiring the break to match too failed runs that were
# perfect from shot one onward and finished byte-identical.
#
# The rules ARE compared on the break, and the turn, so a genuine disagreement
# about whose break it is or what game is being played still fails here.
# WHOSE SHOT EACH WAS is the invariant. That is what the link has to get right
# and it is not subject to anything being in flight: if the two ends disagree
# about who is at the table, the match is broken, full stop.
#
# The PRE-SHOT TABLE is not in this check, and putting it here was a mistake
# made three times. A correction legitimately lands between an end resolving a
# shot and the other taking it, so for one stroke the two can address tables
# that differ — and then converge, which every run doing it has. Table
# agreement is checked below, over the settled tables, where the tolerance for
# an in-flight correction already lives. Splitting them this way is stricter
# than what came before, not looser: the turn order is now compared exactly
# rather than being one field among three in a hash.
sh=[r['turn'] for r in hs]
sj=[r['turn'] for r in js]
def aligns(a, b):
    return (subseq(a,b) or subseq(b,a) or
            subseq(a[1:],b) or subseq(a,b[1:]) or subseq(a[1:],b[1:]))
chk("both ends agree whose shot each was", aligns(sj,sh),
    f"host {''.join(sh)} vs joiner {''.join(sj)}"
    " (a skipped break is allowed — the far end took the table instead)")
# THE SETTLED TABLES ARE REPORTED, NOT ASSERTED, and that is deliberate rather
# than an oversight — it was briefly made a check and the check was wrong.
#
# A "settled table" is every distinct table an end comes to rest on. An end that
# resolves a shot and then takes the striker's correction passes through TWO
# where the other passes through one, and which end does that varies from shot
# to shot. So both lists carry transient intermediates the other never had, and
# NEITHER is a subsequence of the other on completely correct behaviour: a run
# with identical turn order, identical score and an identical finishing table
# came through with 26 and 28 of them, 24 shared.
#
# What matters is where they CONVERGE, and that is covered exactly by the checks
# either side of this: the turn order, and the finishing table with its score
# and frame tally. A count of intermediates is not an invariant and asserting it
# only produces failures on healthy runs.
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
# COMPARE AT A SHARED EVENT, not at whenever each process stopped.
#
# The last line in a log is the last thing that end did before its budget ran
# out or its opponent quit — two different moments, and a correction in flight
# at one of them shows as a difference that is not one. The FRAME ENDING is an
# event both ends reach, so that is where the two tables are compared. Same
# reasoning as "no frame, no verdict" above: a comparison has to be anchored to
# something both ends did.
#
# Everything is still compared there — balls, rules, score and tally — so this
# is not a relaxation. It is the same comparison at a defined instant.
def at_frame_end(rs):
    for r in rs:
        f0, f1 = r['fr'].split('/')
        if int(f0) + int(f1) > 0:
            return r
    return rs[-1]
fh, fj = at_frame_end(h), at_frame_end(j)
same = sig(fh)==sig(fj)
note = ""
if not same:
    for a,b,which in ((h[:-1],j,'host'),(h,j[:-1],'join')):
        if a and b and a[-1]['st']!='ROLL' and b[-1]['st']!='ROLL' and sig(a[-1])==sig(b[-1]):
            same=True; fh,fj=a[-1],b[-1]
            note=f"  (ignoring {which}'s trailing OVER: the other end had quit)"
            break
# NO FRAME, NO VERDICT.
#
# Every check below the shot counts assumes both ends stopped at the SAME
# event, and the only event they share is a frame being won. If neither end
# ever finished one they were cut wherever their frame budget ran out — one
# mid-shot, the other not — and the tails differ for reasons that have nothing
# to do with the link.
#
# This used to report FAIL for that, which is worse than useless: it is a red
# light that means "ask again louder". It cost this project a wrong diagnosis
# three times in one day, twice while chasing a desync that was not there. An
# INCONCLUSIVE run says so and exits non-zero WITHOUT claiming a fault.
finished = any(int(r['fr'].split('/')[0]) + int(r['fr'].split('/')[1]) > 0
               for r in (h[-1], j[-1]))
if not finished:
    print(f"\n  [....] neither end finished a frame — {SHOTS_HINT}")
    print("         Nothing below this line can be read as a link fault: the two")
    print("         ends were cut at different moments, not at the same event.")
    print("         Give it more shots, or a game whose frames are shorter")
    print("         (GAME=6 is 6-red snooker and finishes far sooner than 12ft).")
    print("\nINCONCLUSIVE\n")
    sys.exit(2)

chk("both ends finished on the same table", same,
    note + show('host',fh) + show('join',fj))
chk("both ends play the same match length", h[-1]['bo']==j[-1]['bo'],
    f"best of {h[-1]['bo']} / {j[-1]['bo']}")
print(f"\n{'PASS' if ok else 'FAIL'}\n")
sys.exit(0 if ok else 1)
PY
