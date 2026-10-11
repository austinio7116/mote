#!/bin/sh
# pull-backup.sh: a copy of the relay's data on this machine, once a day --
# off the server, in case the server itself is lost. Without the Meta secret
# (it lives in ~/keys/meta.secret already). Kept 90 days.
#
# Cron tries every hour (WSL only runs while the PC does) and a day already
# pulled is skipped:
#   17 * * * * /home/maustin/thumby-color/mote/relay/backup/pull-backup.sh >> $HOME/mote-relay-backups/pull.log 2>&1
#
# MOTE_BACKUP_DIR overrides where they go (default ~/mote-relay-backups).
set -eu
DST=${MOTE_BACKUP_DIR:-$HOME/mote-relay-backups}
KEEP_DAYS=90
mkdir -p "$DST"
day=$(date -u +%Y-%m-%d)
tmp="$DST/.partial-$day.tar.gz"
out="$DST/mote-relay-$day.tar.gz"
[ -s "$out" ] && exit 0                     # today's is already here
umask 077
ssh -o BatchMode=yes -o ConnectTimeout=20 mote-relay \
    'sudo tar czf - -C /var/lib/private --exclude=mote-relay/meta.secret mote-relay' > "$tmp"
tar tzf "$tmp" | grep -q '^mote-relay/results.jsonl$'     # it really holds the results
mv -f "$tmp" "$out"
find "$DST" -maxdepth 1 -name 'mote-relay-*.tar.gz' -mtime +"$KEEP_DAYS" -delete
echo "$(date -u '+%Y-%m-%d %H:%M:%S') pulled $out ($(stat -c %s "$out") bytes)"
