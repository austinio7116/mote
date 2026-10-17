#!/bin/bash
# deploy_765_relay.sh -- the relay and the stats app for the drop fixes (2026-10-18):
#   * a member who falls behind is not thrown out of a room for the others'
#     movement (0IronSkull0, Orms16: "backlog 262 KB, dropped")
#   * walk-outs before this deploy win nothing; from it on they count again
#     (Mark: "we want walk outs to count from the fix forward") -- the deploy's
#     own time is written into WALKOUTS_COUNT_FROM in both files as they go up
# Run ONLY with Mark's go: restarting the relay drops everyone for a few
# seconds (the club rejoins by itself; a match under way is interrupted).
# Nothing in results.jsonl or any other data file is changed. results.db, the
# honours' index, rebuilds itself from results.jsonl on start (its version
# carries the cut-off); a copy of it is kept first.
#
#   relay/deploy/deploy_765_relay.sh
#
# Undo: copy /opt/mote-relay/{mote_relay.py,mote_stats.py,mote_stats_model.py}.bak-<stamp>
# back, copy /var/lib/private/mote-relay/results.db.bak-<stamp> back, restart
# mote-relay and mote-stats.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -c "import ast; [ast.parse(open(f).read()) for f in ('mote_relay.py', 'mote_stats.py', 'mote_stats_model.py')]"
python3 test_lossy.py | tail -1 | grep -q "^PASS" || { echo "test_lossy.py fails -- not deploying"; exit 1; }
python3 test_honours.py | tail -1 | grep -q " 0 bad" || { echo "test_honours.py fails -- not deploying"; exit 1; }
STAMP=$(date -u +%Y%m%d-%H%M)
FROM=$(date -u +"%Y-%m-%d %H:%M:%S")
H=mote-relay
n=$(ssh $H 'ss -tn state established "( sport = :443 )" | tail -n +2 | wc -l')
[ "$n" = "0" ] || [ "${DEPLOY_ANYWAY:-}" = "1" ] || { echo "not quiet: $n connections -- not deploying"; exit 1; }
echo "connections now: $n"
T=$(mktemp -d)
cp mote_relay.py mote_stats.py mote_stats_model.py "$T"/
sed -i "s/^WALKOUTS_COUNT_FROM = \".*\"/WALKOUTS_COUNT_FROM = \"$FROM\"/" "$T"/mote_relay.py "$T"/mote_stats_model.py
grep -q "^WALKOUTS_COUNT_FROM = \"$FROM\"" "$T"/mote_relay.py && grep -q "^WALKOUTS_COUNT_FROM = \"$FROM\"" "$T"/mote_stats_model.py \
    || { echo "the cut-off was not written -- not deploying"; rm -rf "$T"; exit 1; }
scp -q "$T"/mote_relay.py "$T"/mote_stats.py "$T"/mote_stats_model.py $H:/tmp/
rm -rf "$T"
ssh $H "set -e
for f in mote_relay.py mote_stats.py mote_stats_model.py; do sudo cp -p /opt/mote-relay/\$f /opt/mote-relay/\$f.bak-$STAMP; done
[ -f /var/lib/private/mote-relay/results.db ] && sudo cp -p /var/lib/private/mote-relay/results.db /var/lib/private/mote-relay/results.db.bak-$STAMP
for f in mote_relay.py mote_stats.py mote_stats_model.py; do sudo install -m 644 /tmp/\$f /opt/mote-relay/\$f; rm /tmp/\$f; done
sudo systemctl restart mote-relay
sleep 3
systemctl is-active mote-relay
sudo systemctl restart mote-stats
sleep 2
systemctl is-active mote-stats
sudo journalctl -u mote-relay -n 8 --no-pager | grep -i -E 'traceback|error|results.db' || true
"
echo "deployed $STAMP; walk-outs count from $FROM UTC"
echo "backups: /opt/mote-relay/{mote_relay,mote_stats,mote_stats_model}.py.bak-$STAMP, results.db.bak-$STAMP"
echo "now set WALKOUTS_COUNT_FROM = \"$FROM\" in relay/mote_relay.py and relay/mote_stats_model.py and commit it"
