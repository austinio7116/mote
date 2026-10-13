#!/bin/bash
# stats_only.sh -- the stats app (mote_stats.py, mote_stats_model.py,
# mote_stats.html) onto mote-relay, and only it restarted. Games never talk to
# the stats app, so this is safe at any time, people on or not.
#
#   relay/deploy/stats_only.sh
#
# Undo: the old files are kept as *.bak-<stamp>; copy them back and restart
# mote-stats.
set -euo pipefail
cd "$(dirname "$0")/.."
STAMP=$(date -u +%Y%m%d-%H%M)
H=mote-relay
F="mote_stats.py mote_stats_model.py mote_stats.html"
scp -q $F $H:/tmp/
ssh $H "set -e
for f in $F; do sudo cp -p /opt/mote-relay/\$f /opt/mote-relay/\$f.bak-$STAMP; sudo install -m 644 /tmp/\$f /opt/mote-relay/\$f; rm /tmp/\$f; done
sudo systemctl restart mote-stats
sleep 3
systemctl is-active mote-stats
"
echo "stats app deployed $STAMP; backups /opt/mote-relay/mote_stats*.bak-$STAMP"
