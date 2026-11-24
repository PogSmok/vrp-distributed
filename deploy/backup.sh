#!/bin/sh
# Database backups (ADR 0019): `backup.sh loop|now|verify|restore <file>`.
set -eu

KEEP=14
export PGHOST=postgres PGUSER=vrp PGDATABASE=vrp
PGPASSWORD=$(cat /run/vrp/password)
export PGPASSWORD

dump() {
    file="/backups/vrp-$(date -u +%Y%m%dT%H%M%SZ).dump"
    # `set -e` does not apply when the caller tests this function.
    pg_dump --format=custom --file="$file.part" || {
        rm -f "$file.part"
        return 1
    }
    mv "$file.part" "$file" || return 1
    ls -1t /backups/vrp-*.dump | tail -n "+$((KEEP + 1))" | xargs -r rm -f
    echo "backed up to $(basename "$file")"
}

counts() {
    psql --dbname="$1" -Atc \
        "SELECT (SELECT count(*) FROM graphs), (SELECT count(*) FROM jobs),
                (SELECT count(*) FROM tasks), (SELECT count(*) FROM islands)"
}

# Restores the latest backup into a scratch database.
verify() {
    file=$(ls -1t /backups/vrp-*.dump | head -n 1)
    dropdb --if-exists vrp_verify
    createdb vrp_verify
    pg_restore --no-owner --exit-on-error --dbname=vrp_verify "$file"
    restored=$(counts vrp_verify)
    dropdb vrp_verify
    echo "$(basename "$file") restores: graphs|jobs|tasks|islands = $restored"
    echo "the live database has:        graphs|jobs|tasks|islands = $(counts vrp)"
}

# Every night at 02:00 UTC. A failed night is logged, and the next one runs.
loop() {
    while true; do
        now=$(date -u +%s)
        next=$(date -u -d "02:00" +%s)
        [ "$next" -gt "$now" ] || next=$((next + 86400))
        sleep $((next - now))
        dump || echo "the backup failed" >&2
    done
}

case "${1:-loop}" in
    loop) loop ;;
    now) dump ;;
    verify) verify ;;
    restore) pg_restore --clean --if-exists --no-owner --exit-on-error \
        --dbname=vrp "/backups/$(basename "$2")" ;;
    *) echo "usage: backup.sh loop|now|verify|restore <file>" >&2; exit 2 ;;
esac
