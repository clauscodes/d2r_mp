#!/bin/sh
# On the server, in the clone: pull, rebuild, restart, check.
set -eu
cd "$(dirname "$0")"
[ -f .env ] || { echo ".env missing - copy .env.example and set ROOMS_DOMAIN"; exit 1; }
git pull --ff-only
docker compose up -d --build
sleep 3
docker compose exec -T rooms python -c "import urllib.request; print(urllib.request.urlopen('http://127.0.0.1:8080/healthz').read().decode())"
