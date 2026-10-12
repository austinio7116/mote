#!/bin/bash
# deploy_75_tourn.sh -- the relay and stats app for 7.5's tournaments, rejoin and
# the club's kept places, onto mote-relay. Run ONLY when the relay is quiet
# (Mark: "wait for quiet"); nothing under /var/lib/mote-relay is touched.
#
#   relay/deploy/deploy_75_tourn.sh
#
# What it does, and how to undo it:
#   * backs up /opt/mote-relay/{mote_relay.py,mote_tourn.py,mote_stats.py,mote_stats.html}
#     as *.bak-<stamp>                    undo: copy them back, restart
#   * makes /etc/mote-relay/admin.token (root, 600) if there is none -- never printed
#   * adds drop-ins /etc/systemd/system/mote-{relay,stats}.service.d/tourn.conf:
#     the token as a credential; the relay gets --admin-token-file, the stats
#     app --tourn-token-file               undo: delete both drop-ins, daemon-reload, restart
#   * copies the new files, restarts both, and checks them
set -euo pipefail
cd "$(dirname "$0")/.."
STAMP=$(date -u +%Y%m%d-%H%M)
H=mote-relay
n=$(ssh $H 'ss -tn state established "( sport = :443 )" | tail -n +2 | wc -l')
[ "$n" = "0" ] || { echo "not quiet: $n connections -- not deploying"; exit 1; }
scp -q mote_relay.py mote_tourn.py mote_stats.py mote_stats.html $H:/tmp/
ssh $H "set -e
sudo mkdir -p /etc/mote-relay
[ -f /etc/mote-relay/admin.token ] || (umask 077; openssl rand -hex 24 | sudo tee /etc/mote-relay/admin.token >/dev/null)
sudo chmod 600 /etc/mote-relay/admin.token
for f in mote_relay.py mote_tourn.py mote_stats.py mote_stats.html; do sudo cp -p /opt/mote-relay/\$f /opt/mote-relay/\$f.bak-$STAMP; done
sudo mkdir -p /etc/systemd/system/mote-relay.service.d /etc/systemd/system/mote-stats.service.d
sudo tee /etc/systemd/system/mote-relay.service.d/tourn.conf >/dev/null <<'EOF'
[Service]
LoadCredential=admin:/etc/mote-relay/admin.token
ExecStart=
ExecStart=/usr/bin/python3 /opt/mote-relay/mote_relay.py --port 443 --store /var/lib/mote-relay --meta-secret-file /var/lib/mote-relay/meta.secret --admin-token-file \${CREDENTIALS_DIRECTORY}/admin
EOF
sudo tee /etc/systemd/system/mote-stats.service.d/tourn.conf >/dev/null <<'EOF'
[Service]
LoadCredential=tourn:/etc/mote-relay/admin.token
ExecStart=
ExecStart=/usr/bin/python3 /opt/mote-relay/mote_stats.py --store /data --port 8443 --relay 127.0.0.1:443 --state /var/lib/mote-stats --backups /backups --token-file \${CREDENTIALS_DIRECTORY}/token --cert \${CREDENTIALS_DIRECTORY}/cert.pem --key \${CREDENTIALS_DIRECTORY}/key.pem --tourn-token-file \${CREDENTIALS_DIRECTORY}/tourn
EOF
for f in mote_relay.py mote_tourn.py mote_stats.py mote_stats.html; do sudo install -m 644 /tmp/\$f /opt/mote-relay/\$f; rm /tmp/\$f; done
sudo systemctl daemon-reload
sudo systemctl restart mote-relay mote-stats
sleep 3
systemctl is-active mote-relay mote-stats
sha256sum /opt/mote-relay/mote_relay.py /opt/mote-relay/mote_stats.py | cut -c1-12
sudo journalctl -u mote-relay -n 4 --no-pager | cut -c1-160
python3 -c \"
import socket
s=socket.create_connection(('127.0.0.1',443),timeout=5); s.sendall(b'MOTE2 ROOMN 1129661808 HELD 1\n'); print('HELD ->', s.makefile().readline().strip())
s=socket.create_connection(('127.0.0.1',443),timeout=5); s.sendall(b'MOTE2 TOURN 1129661808 LIST\n'); print('LIST ->', s.makefile().readline().strip())
\"
"
echo "deployed $STAMP; backups /opt/mote-relay/*.bak-$STAMP"
