#!/usr/bin/env bash
# Download Droid Sans Fallback Full (Apache-2.0), the recommended run-time CJK
# font, from AOSP at a pinned commit, verify it, and write it to the path given.
# Copy the result to the microSD card as fonts/cjk.ttf; see main/fonts/README.md.
#
#   tools/fetch_cjk_font.sh /media/$USER/SDCARD/fonts/cjk.ttf
set -euo pipefail

if [ "$#" -ne 1 ] || [ -z "$1" ]; then
    echo "usage: $0 <output path, e.g. /media/\$USER/SDCARD/fonts/cjk.ttf>" >&2
    exit 2
fi
output="$1"

# platform/frameworks/base at tag android-15.0.0_r1; the file is git blob
# 68641aad1d1b02922fc1ca61fdcd119cb051ef06. Gitiles serves files base64-encoded.
commit="4e43ad12e1b211f15152e9c5b16b0fe88e6b93f3"
url="https://android.googlesource.com/platform/frameworks/base/+/${commit}/data/fonts/DroidSansFallbackFull.ttf?format=TEXT"
sha256="2392015530438bafc48edfc4aee6d9de2387f627a6134d8ab3dfcc99d21c8240"
size=4033576

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

curl -fsSL -o "${work}/font.b64" "${url}"
base64 -d "${work}/font.b64" >"${work}/cjk.ttf"
echo "${sha256}  ${work}/cjk.ttf" | sha256sum -c --quiet -
if [ "$(wc -c <"${work}/cjk.ttf")" -ne "${size}" ]; then
    echo "unexpected size" >&2
    exit 1
fi

mkdir -p "$(dirname "${output}")"
cp "${work}/cjk.ttf" "${output}.part"
mv "${output}.part" "${output}"
echo "Wrote ${output} (DroidSansFallbackFull.ttf, ${size} bytes, SHA-256 ${sha256})"
