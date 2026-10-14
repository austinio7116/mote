#!/bin/sh
# mote-relay-backup: one snapshot a day of the relay's whole data folder
# (share codes, results, disputes, reports, salt, Meta secret), kept 30 days in
# /var/backups/mote-relay. Reads the data, never changes it. Run by
# mote-relay-backup.timer as root; installed as /usr/local/sbin/mote-relay-backup.
#
# The folder is 755 so the stats page can list the snapshots; each snapshot is
# 600 (root only) because it holds the Meta secret.
#
# Restore (stop the relay first, keep what is there):
#   sudo systemctl stop mote-relay
#   sudo mv /var/lib/private/mote-relay /var/lib/private/mote-relay.before-restore
#   sudo tar xzf /var/backups/mote-relay/mote-relay-YYYY-MM-DD.tar.gz -C /var/lib/private
#   sudo systemctl start mote-relay
set -eu
SRC_PARENT=/var/lib/private
DST=/var/backups/mote-relay
KEEP_DAYS=30
mkdir -p "$DST"
chmod 755 "$DST"
day=$(date -u +%Y-%m-%d)
tmp="$DST/.partial-$day.tar.gz"
out="$DST/mote-relay-$day.tar.gz"
umask 077
tar czf "$tmp" -C "$SRC_PARENT" mote-relay
tar tzf "$tmp" > /dev/null                  # a snapshot that cannot be read back is not one
mv -f "$tmp" "$out"
find "$DST" -maxdepth 1 -name 'mote-relay-*.tar.gz' -mtime +"$KEEP_DAYS" -delete
echo "mote-relay-backup: $out ($(stat -c %s "$out") bytes)"
# PLAYERS' BACKUPS (CueVR 7.6), WEEKLY (Mark: "weekly is fine"): on Sundays,
# kept five weeks -- apart from the relay's own daily one, which they would
# otherwise swell. Only if the folder exists (the relay makes it).
if [ "$(date -u +%u)" = "7" ] && [ -d "$SRC_PARENT/mote-backups" ]; then
    wtmp="$DST/.partial-backups-$day.tar.gz"
    wout="$DST/mote-backups-$day.tar.gz"
    tar czf "$wtmp" -C "$SRC_PARENT" mote-backups
    tar tzf "$wtmp" > /dev/null
    mv -f "$wtmp" "$wout"
    find "$DST" -maxdepth 1 -name 'mote-backups-*.tar.gz' -mtime +35 -delete
    echo "mote-relay-backup: $wout ($(stat -c %s "$wout") bytes)"
fi
