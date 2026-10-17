#!/bin/bash
# deploy_764_relay.sh -- the relay for CueVR 7.6.4 onto mote-relay: a player's
# tournaments (HISTORY, STATE) routed to the organiser, and the honours answer
# carrying each proved account's tournament titles (TWON, for its cabinet)
# and the club's TOURNAMENT WINNERS (TCHAMP). Run ONLY when the relay is quiet;
# nothing under /var/lib/mote-relay is touched.
#
#   relay/deploy/deploy_764_relay.sh
#
# Undo: copy /opt/mote-relay/{mote_relay.py,mote_tourn.py}.bak-<stamp> back and
# restart mote-relay.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -c "import ast; [ast.parse(open(f).read()) for f in ('mote_relay.py', 'mote_tourn.py')]"
python3 test_tourn.py | tail -1 | grep -q " 0 bad" || { echo "test_tourn.py fails -- not deploying"; exit 1; }
STAMP=$(date -u +%Y%m%d-%H%M)
H=mote-relay
n=$(ssh $H 'ss -tn state established "( sport = :443 )" | tail -n +2 | wc -l')
[ "$n" = "0" ] || [ "${DEPLOY_ANYWAY:-}" = "1" ] || { echo "not quiet: $n connections -- not deploying"; exit 1; }
echo "connections now: $n"
scp -q mote_relay.py mote_tourn.py $H:/tmp/
ssh $H "set -e
for f in mote_relay.py mote_tourn.py; do sudo cp -p /opt/mote-relay/\$f /opt/mote-relay/\$f.bak-$STAMP; sudo install -m 644 /tmp/\$f /opt/mote-relay/\$f; rm /tmp/\$f; done
sudo systemctl restart mote-relay
sleep 3
systemctl is-active mote-relay
sudo journalctl -u mote-relay -n 6 --no-pager | grep -i -E 'traceback|error' || true
"
echo "relay deployed $STAMP; backups /opt/mote-relay/mote_{relay,tourn}.py.bak-$STAMP"
