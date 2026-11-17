#!/usr/bin/env bash
# Regenerate the Selawik LVGL bitmap fonts in main/fonts/ from the official
# microsoft/Selawik 1.01 release. See main/fonts/README.md for provenance.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

release_url="https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip"
release_sha256="3f62c51e05e3b5a1e6241cf92a371f0be2ea1183aa87b30718bbd40832a8d423"
lv_font_conv="lv_font_conv@1.5.3"
src_dir="build/selawik-src"

mkdir -p "${src_dir}"
zip_path="${src_dir}/Selawik_Release.zip"
if [ ! -f "${zip_path}" ]; then
    curl -fsSL -o "${zip_path}" "${release_url}"
fi
echo "${release_sha256}  ${zip_path}" | sha256sum -c -
unzip -o -q -j "${zip_path}" selawk.ttf selawksl.ttf selawkl.ttf -d "${src_dir}"
sha256sum -c - <<EOF
e9d98518d8ac2817782a9a382430463a2e0793ea68350b695bb727d9a830ee1c  ${src_dir}/selawk.ttf
8620960344f12093482cded984a1aafa5a57d24cbf7e9299da125ccf0e9d4102  ${src_dir}/selawksl.ttf
8e19d073091a1e869b5b0d48b925e605c0a4c6ece7df7b22a364fb065314c4c5  ${src_dir}/selawkl.ttf
EOF

# size  ttf            (weight)
fonts=(
    "14 selawk.ttf"    # Regular: body text, row details
    "18 selawk.ttf"    # Regular: buttons, labels, list titles
    "22 selawksl.ttf"  # Semilight: overlay and page titles
    "28 selawkl.ttf"   # Light: now-playing title, SETTINGS header
)

for entry in "${fonts[@]}"; do
    read -r size ttf <<<"${entry}"
    npx --yes "${lv_font_conv}" \
        --font "${src_dir}/${ttf}" -r 0x20-0x7E -r 0xA0-0xFF \
        --size "${size}" --format lvgl --bpp 4 --no-compress \
        --lv-fallback "lv_font_montserrat_${size}" \
        --lv-font-name "ui_font_selawik_${size}" \
        -o "main/fonts/ui_font_selawik_${size}.c"
done
