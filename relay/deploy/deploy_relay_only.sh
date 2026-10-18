#!/bin/bash
# deploy_relay_only.sh -- mote_relay.py alone onto mote-relay, as committed
# (WALKOUTS_COUNT_FROM unchanged), when the relay is quiet. Nothing under
# /var/lib/mote-relay is touched; results.db rebuilds itself only if its
# version changed.
#
#   relay/deploy/deploy_relay_only.sh
#
# Undo: copy /opt/mote-relay/mote_relay.py.bak-<stamp> back and restart mote-relay.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -c "import ast; ast.parse(open('mote_relay.py').read())"
for t in test_lossy.py test_forfeit_leaver.py; do
    python3 $t | tail -1 | grep -q "^PASS" || { echo "$t fails -- not deploying"; exit 1; }
done
python3 test_resume.py | tail -1 | grep -q " 0 bad" || { echo "test_resume.py fails -- not deploying"; exit 1; }
STAMP=$(date -u +%Y%m%d-%H%M)
H=mote-relay
n=$(ssh $H 'ss -tn state established "( sport = :443 )" | tail -n +2 | wc -l')
[ "$n" = "0" ] || [ "${DEPLOY_ANYWAY:-}" = "1" ] || { echo "not quiet: $n connections -- not deploying"; exit 1; }
echo "connections now: $n"
grep '^WALKOUTS_COUNT_FROM' mote_relay.py
scp -q mote_relay.py $H:/tmp/
ssh $H "set -e
sudo cp -p /opt/mote-relay/mote_relay.py /opt/mote-relay/mote_relay.py.bak-$STAMP
sudo install -m 644 /tmp/mote_relay.py /opt/mote-relay/mote_relay.py; rm /tmp/mote_relay.py
sudo systemctl restart mote-relay
sleep 3
systemctl is-active mote-relay
sudo journalctl -u mote-relay -n 6 --no-pager | grep -i -E 'traceback|error|results.db' || true
"
echo "relay deployed $STAMP; backup /opt/mote-relay/mote_relay.py.bak-$STAMP"
