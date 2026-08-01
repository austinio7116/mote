#!/usr/bin/env bash
# Motebox — the Empire Audit (DESIGN.md 17).
#
# A god sim's bugs are STATISTICAL, not structural: nothing crashes, the world
# just quietly becomes wrong — a population that collapses, a war against a
# kingdom that no longer exists, a plague that never ends, a Faith reserve that
# grows to a hundred thousand. None of those show up in a screenshot, and all four
# of them were really in this game until a curve showed them.
#
# So the audit runs worlds headlessly and asserts the invariants over the CSV that
# MOTEBOX_YEARS prints. Every check below exists because it once failed.
#
#   ./authoring/audit.sh              # the standard sweep
#   ./authoring/audit.sh 12 800       # 12 seeds, 800 years each
set -u
cd "$(dirname "$0")/../../.."          # the mote root

SEEDS=${1:-8}
YEARS=${2:-400}
FAIL=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Five founding parties, placed on LAND by the game (MOTEBOX_SEEDN) rather than at
# fixed coordinates. With fixed coordinates, ocean-heavy worlds put every party in
# the sea, and half the audited worlds ran four hundred years empty and passed.
DROPS=5

echo "Motebox Empire Audit — $SEEDS seeds x $YEARS years"
echo

for i in $(seq 1 "$SEEDS"); do
    SEED=$((i * 7919))
    OUT="$TMP/s$SEED.csv"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy MOTE_AUTORUN=1 MOTE_DT_MS=33 \
        MOTEBOX_SEED=$SEED MOTEBOX_SEEDN="$DROPS" MOTEBOX_YEARS=$YEARS \
        MOTE_SHOT=/dev/null MOTE_SHOT_FRAME=2 \
        ./tools/mote run games/motebox 2>"$OUT" >/dev/null

    grep -E '^[0-9]+,' "$OUT" > "$TMP/data.csv" || true
    if [ ! -s "$TMP/data.csv" ]; then
        echo "seed $SEED: FAIL — no CSV produced (crash?)"
        FAIL=$((FAIL + 1)); continue
    fi

    awk -F, -v seed="$SEED" -v years="$YEARS" '
    # columns: year,pop,wild,villages,kingdoms,wars,faith,ruin,age,
    #          d_age,d_wound,d_eaten,d_slain,d_disaster,d_plague,d_starved
    {
        y=$1; pop=$2; wild=$3; v=$4; k=$5; wars=$6; faith=$7; ruin=$8
        dstarve=$16; dplague=$15; dslain=$13; ddis=$14
        # every cause, so a share of the dying can be taken: age, wounds, eaten, slain,
        # disaster, plague, starvation
        alldeath=$10+$11+$12+$13+$14+$15+$16
        if (pop > 20) everlived = 1
        if (y > 30 && pop == 0 && everlived) zeropop++
        if (dslain > maxslain) maxslain = dslain
        if (ddis > maxdis) maxdis = ddis
        if (y > 30 && wild == 0) zerowild++
        if (k > 12)              badk++
        if (v > 48)              badv++
        if (wars > 0 && k < 2)   ghostwar++
        if (faith > 6000)        richfaith++
        if (faith < 0)           negfaith++
        # the population must not be a one-way ramp: a healthy world oscillates
        if (pop > maxpop) maxpop = pop
        if (y == years - 1) { endpop = pop; endv = v; endk = k
                              endstarve = dstarve; endplague = dplague; deaths = alldeath }
        lastplague = dplague; n++
        if (y >= years/2) { late_plague_delta += dplague - prev_plague }
        prev_plague = dplague
    }
    END {
        bad = 0
        # 1. the world must still be inhabited (it survived four hundred years)
        # A world CAN die, and one of ten audited worlds did: a war took 72 of its
        # 60-odd people and an Age of Sun heatwave finished the survivors. That is
        # WorldBox behaving as intended, so the invariant is not "never dies" but
        # "never dies of NOTHING": an extinction with no war and no disaster behind
        # it means the civilisation simply failed to sustain itself, which is the
        # bug class this audit is for. (Seeds where the founders landed in ocean
        # never lived at all and are not extinctions either.)
        if (zeropop > 0 && maxslain < 10 && maxdis < 10) {
            printf "  quiet extinction: %d years with zero civ pop, and nothing killed them\n", zeropop
            bad++
        } else if (zeropop > 0) {
            printf "  (died out after %d slain and %d lost to disaster — a story, not a fault)\n",
                   maxslain, maxdis
        }
        # 2. the ecology must not be wiped out by the class caps overlapping
        if (zerowild > 0) { printf "  ecology dead: %d years with zero wildlife\n", zerowild; bad++ }
        # 3. bookkeeping bounds
        if (badk) { printf "  kingdom count exceeded MAXK\n"; bad++ }
        if (badv) { printf "  village count exceeded MAXV\n"; bad++ }
        # 4. no war against a kingdom that does not exist
        if (ghostwar) { printf "  ghost war: %d years at war with under two kingdoms\n", ghostwar; bad++ }
        # 5. Faith is a flow with a ceiling, not a hoard
        if (richfaith) { printf "  faith exceeded its cap (%d years)\n", richfaith; bad++ }
        if (negfaith)  { printf "  faith went negative\n"; bad++ }
        # 6. a plague has to BURN OUT: deaths in the second half must not still be
        #    accelerating, or it is not an epidemic, it is a slow extinction
        if (late_plague_delta > years * 4) {
            printf "  endemic plague: %d deaths across the second half\n", late_plague_delta; bad++
        }
        # 7. starvation must be rare — a village beside a full barn should not starve.
        #    AGAINST THE PEAK, NOT THE END. Measured against the final population this fired on
        #    any world that died of something else: a world lost to a war (239 slain) reported
        #    "famine: 45 starvation deaths against a final pop of 0" and failed the audit, when
        #    45 across four centuries is nothing. What the invariant means is that starvation
        #    is not a major cause of death here, and the size of a world is its PEAK.
        #    (No apostrophes in this file: the awk program is inside a single-quoted string.)
        # 7b. AS A SHARE OF THE DYING, not as a raw total. Against maxpop/2+40 this compared a
        #     FOUR-HUNDRED-YEAR TOTAL with a snapshot, so a long healthy world accumulated its
        #     way over the line: measured, one world in eight tripped it whichever way the
        #     carriers walked, at 278 deaths in one build and 361 in the other, in worlds that
        #     were at the population cap and growing. What the check is FOR is stated in its
        #     own comment — that starvation is not a major cause of death here — so that is
        #     what it now measures. A third of all deaths is a famine; a tenth is attrition.
        if (deaths > 60 && endstarve * 100 > deaths * 33) {
            printf "  famine: %d of %d deaths were starvation (%d%%), peak pop %d\n",
                   endstarve, deaths, endstarve * 100 / deaths, maxpop; bad++
        }
        if (bad == 0)
            printf "seed %-7s ok   end: pop %-4d villages %-3d kingdoms %-2d  peak pop %d\n",
                   seed, endpop, endv, endk, maxpop
        else
            printf "seed %-7s FAIL (%d)\n", seed, bad
        exit bad ? 1 : 0
    }' "$TMP/data.csv" || FAIL=$((FAIL + 1))
done

echo
if [ "$FAIL" -eq 0 ]; then
    echo "PASS — $SEEDS worlds, $YEARS years each, all invariants held"
else
    echo "FAIL — $FAIL of $SEEDS worlds broke an invariant"
fi
exit $FAIL
