#!/bin/sh
# Entry point of the KestroDB image: kestro-server with settings from the environment.
# Arguments starting with - are added to the server's options; any other command runs instead
# (docker run -it kestrodb bash).
set -e
if [ "${1:-}" = kestro-server ]; then shift; fi
case "${1:-}" in
  ''|-*) ;;
  *) exec "$@" ;;
esac
set -- --db "${KESTRODB_DB:-/data/kestro.kdb}" --create --tcp "${KESTRODB_LISTEN:-0.0.0.0:7820}" "$@"
# Authentication: a users file (roles), or one admin token from a file (a Docker secret) or
# KESTRODB_TOKEN (read by the server itself).
if [ -n "${KESTRODB_USERS_FILE:-}" ]; then
  set -- "$@" --users "$KESTRODB_USERS_FILE"
elif [ -n "${KESTRODB_TOKEN_FILE:-}" ]; then
  set -- "$@" --token-file "$KESTRODB_TOKEN_FILE"
fi
# exec: the server is PID 1 and gets docker stop's SIGTERM itself (it stops cleanly on it).
exec kestro-server "$@"
