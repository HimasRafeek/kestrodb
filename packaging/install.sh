#!/bin/sh
# Installs a KestroDB Linux release as a systemd service:
#   sudo ./packaging/install.sh                       (from an unpacked kestrodb-<version>-linux-<arch>)
#   sudo sh install.sh kestrodb-0.9.0-linux-x64.tar.gz
# Options: --dry-run (print what it would do, change nothing; needs no root), --prefix DIR
# (default /opt/kestrodb), --no-start (install and enable, do not start).
# It makes: the release in PREFIX (the previous one kept as PREFIX.old), a kestrodb system user,
# /var/lib/kestrodb (the database), /etc/kestrodb/kestrodb.env (settings) and
# /etc/kestrodb/token (a new random admin token, unless one exists), /usr/local/bin/kestro-server,
# and /etc/systemd/system/kestrodb.service. Existing settings, token and data are never touched.
# Needs: glibc 2.34+ and OpenSSL 3 (libssl3 / openssl-libs), as on Ubuntu 22.04+, Debian 12+,
# RHEL/Rocky/Alma 9+, Fedora 35+, Amazon Linux 2023.
# Uninstall: systemctl disable --now kestrodb; rm -r PREFIX /etc/systemd/system/kestrodb.service
# /usr/local/bin/kestro-server (and, to delete the data, /var/lib/kestrodb /etc/kestrodb; userdel kestrodb).
set -eu
PREFIX=/opt/kestrodb DRY=0 START=1 ARCHIVE=
while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) DRY=1 ;;
    --no-start) START=0 ;;
    --prefix) PREFIX=$2; shift ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    -*) echo "install.sh: unknown option $1" >&2; exit 2 ;;
    *) ARCHIVE=$1 ;;
  esac
  shift
done
say() { echo "install.sh: $*"; }
die() { echo "install.sh: $*" >&2; exit 1; }
run() { if [ $DRY = 1 ]; then echo "  would run: $*"; else "$@"; fi; }
[ $DRY = 1 ] || [ "$(id -u)" = 0 ] || die "run as root (sudo), or with --dry-run"

# The release: the archive given, or the one this script is part of.
if [ -n "$ARCHIVE" ]; then
  [ -f "$ARCHIVE" ] || die "no file $ARCHIVE"
  TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
  tar -xzf "$ARCHIVE" -C "$TMP"
  SRC=$(find "$TMP" -mindepth 1 -maxdepth 1 -type d | head -1)
else
  SRC=$(cd "$(dirname "$0")/.." && pwd)
fi
[ -x "$SRC/bin/kestro-server" ] || die "$SRC is not a KestroDB Linux release (no bin/kestro-server)"

# The machine and the system libraries the binaries need.
elf=$(od -An -tx1 -j18 -N1 "$SRC/bin/kestro-server" | tr -d ' ')
case "$elf:$(uname -m)" in
  3e:x86_64|b7:aarch64|b7:arm64) ;;
  *) die "this release is for $( [ "$elf" = b7 ] && echo arm64 || echo x86-64 ), this machine is $(uname -m)" ;;
esac
glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')
[ -n "$glibc" ] || die "no glibc found (musl/Alpine): build from source, or use the Docker image"
[ "$(printf '%s\n2.34\n' "$glibc" | sort -V | head -1)" = 2.34 ] || die "glibc $glibc is too old (2.34 or newer needed): build from source"
if ! ldconfig -p 2>/dev/null | grep -q 'libssl\.so\.3 '; then
  say "warning: no OpenSSL 3 (libssl.so.3) found: install libssl3 (Debian/Ubuntu) or openssl-libs (RHEL/Fedora) before starting"
fi
say "installing $(basename "$SRC") to $PREFIX (glibc $glibc)"

# Files.
if [ -d "$PREFIX" ]; then run rm -rf "$PREFIX.old"; run mv "$PREFIX" "$PREFIX.old"; fi
run mkdir -p "$(dirname "$PREFIX")"
run cp -a "$SRC" "$PREFIX"
run chown -R root:root "$PREFIX"
run ln -sf "$PREFIX/bin/kestro-server" /usr/local/bin/kestro-server

# User, data and settings.
if ! id kestrodb >/dev/null 2>&1; then
  nologin=$(command -v nologin || echo /usr/sbin/nologin)
  run useradd --system --user-group --home-dir /var/lib/kestrodb --no-create-home --shell "$nologin" kestrodb
fi
run install -d -o kestrodb -g kestrodb -m 0750 /var/lib/kestrodb
run install -d -o root -g kestrodb -m 0750 /etc/kestrodb
if [ ! -f /etc/kestrodb/kestrodb.env ]; then
  if [ $DRY = 1 ]; then echo "  would write /etc/kestrodb/kestrodb.env"; else
    cat > /etc/kestrodb/kestrodb.env <<'EOF'
# KestroDB service settings (systemctl restart kestrodb after a change). systemd reads this
# file (EnvironmentFile); keep values in double quotes, so a shell can source it too.
KESTRODB_DB="/var/lib/kestrodb/kestro.kdb"
# HOST:PORT; 0.0.0.0:7820 for every interface (keep authentication on, and consider TLS).
KESTRODB_LISTEN="127.0.0.1:7820"
# --token-file FILE (one admin token) or --users FILE (users and roles, see README).
KESTRODB_AUTH="--token-file /etc/kestrodb/token"
# Any other kestro-server options, for example:
# KESTRODB_OPTS="--backup-dir /var/lib/kestrodb/backups --backup-every 1h --backup-keep 7"
KESTRODB_OPTS=""
EOF
    chmod 0640 /etc/kestrodb/kestrodb.env; chgrp kestrodb /etc/kestrodb/kestrodb.env
  fi
fi
if [ ! -f /etc/kestrodb/token ] && [ ! -f /etc/kestrodb/users.txt ]; then
  if [ $DRY = 1 ]; then echo "  would write a new random token to /etc/kestrodb/token"; else
    (umask 027; od -An -N24 -tx1 /dev/urandom | tr -d ' \n' > /etc/kestrodb/token; echo >> /etc/kestrodb/token)
    chgrp kestrodb /etc/kestrodb/token
    say "new admin token in /etc/kestrodb/token (clients: --token-file / KESTRODB_TOKEN)"
  fi
fi

# The service.
unit=/etc/systemd/system/kestrodb.service
if [ $DRY = 1 ]; then echo "  would write $unit (ExecStart $PREFIX/bin/kestro-server)"; else
  sed "s#/opt/kestrodb/#$PREFIX/#g" "$SRC/packaging/systemd/kestrodb.service" > "$unit"
  chmod 0644 "$unit"
fi
if [ -d /run/systemd/system ]; then
  run systemctl daemon-reload
  run systemctl enable kestrodb
  [ $START = 0 ] || run systemctl restart kestrodb  # starts it, or restarts it on the new release
  say "done: systemctl status kestrodb"
else
  say "systemd is not running here: the unit is installed but not started (start kestro-server another way)"
fi
