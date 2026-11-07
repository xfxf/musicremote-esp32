#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
env_file="${project_dir}/.env"

if [[ ! -f "${env_file}" ]]; then
  echo "Missing ${env_file}; copy .env.example and fill it in." >&2
  exit 1
fi

set -a
# shellcheck disable=SC1090
source "${env_file}"
set +a

: "${WIFI_SSID:?WIFI_SSID is required}"
: "${WIFI_PASSWORD:?WIFI_PASSWORD is required}"
: "${AIMP_HOST:?AIMP_HOST is required}"
: "${AIMP_PORT:?AIMP_PORT is required}"
: "${AIMP_PASSWORD:?AIMP_PASSWORD is required}"

export MUSICREMOTE_WIFI_SSID="${WIFI_SSID}"
export MUSICREMOTE_WIFI_PASSWORD="${WIFI_PASSWORD}"
export MUSICREMOTE_AIMP_HOST="${AIMP_HOST}"
export MUSICREMOTE_AIMP_PORT="${AIMP_PORT}"
export MUSICREMOTE_AIMP_PASSWORD="${AIMP_PASSWORD}"

cd "${project_dir}"
idf.py set-target esp32s3

echo "Configured ESP-IDF build for ${AIMP_HOST}:${AIMP_PORT}; credentials remain outside git."
