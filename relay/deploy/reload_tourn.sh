#!/bin/bash
# reload_tourn.sh -- a new mote_tourn.py onto mote-relay WITHOUT a restart:
# copied in, then the running relay told to RELOAD it (admin token, read on
# the server and never printed). Every connection, club, room and match
# carries on. Safe at any time, people on or not (Mark, 2026-10-13: "update
# the server without killing games or requiring we empty the club").
#
#   relay/deploy/reload_tourn.sh
#
# Only for mote_tourn.py. A change to mote_relay.py still needs a restart
# (deploy_75_tourn.sh, at quiet).
#
# Undo: the old file is kept as mote_tourn.py.bak-<stamp>; copy it back and
# run the RELOAD line below again.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -c "import ast; ast.parse(open('mote_tourn.py').read())"
python3 test_tourn.py | tail -1 | grep -q " 0 bad" || { echo "test_tourn.py fails -- not reloading"; exit 1; }
STAMP=$(date -u +%Y%m%d-%H%M)
H=mote-relay
scp -q mote_tourn.py $H:/tmp/mote_tourn.py
ssh $H "set -e
sudo cp -p /opt/mote-relay/mote_tourn.py /opt/mote-relay/mote_tourn.py.bak-$STAMP
sudo install -m 644 /tmp/mote_tourn.py /opt/mote-relay/mote_tourn.py; rm /tmp/mote_tourn.py
sudo python3 -c \"
import socket
tok = open('/etc/mote-relay/admin.token').read().strip()
s = socket.create_connection(('127.0.0.1', 443), timeout=10)
s.sendall(('MOTE2 TOURN 1129661808 RELOAD ' + tok + '\n').encode())
print(s.makefile().readline().strip())
\"
sudo journalctl -u mote-relay -n 3 --no-pager | grep reload | cut -c1-200 || true
"
echo "backup /opt/mote-relay/mote_tourn.py.bak-$STAMP"
