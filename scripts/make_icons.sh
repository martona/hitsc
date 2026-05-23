#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage: make_icons.sh THICK.svg THIN.svg OUTPUT.ico

Creates OUTPUT.ico and a sibling Qt PNG directory named OUTPUT-stem-png.

Low-resolution icons are rendered from THICK.svg.
High-resolution icons are rendered from THIN.svg.

Required tools:
  rsvg-convert
  icotool
EOF
}

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: required tool not found: $1" >&2
        exit 127
    fi
}

render_png() {
    local source_svg=$1
    local size=$2
    local output_png=$3

    rsvg-convert \
        --format=png \
        --keep-aspect-ratio \
        --width="$size" \
        --height="$size" \
        --output="$output_png" \
        "$source_svg"
}

if [[ $# -ne 3 ]]; then
    usage
    exit 64
fi

thick_svg=$1
thin_svg=$2
output_ico=$3

require_tool rsvg-convert
require_tool icotool

if [[ ! -f "$thick_svg" ]]; then
    echo "error: thick SVG not found: $thick_svg" >&2
    exit 66
fi

if [[ ! -f "$thin_svg" ]]; then
    echo "error: thin SVG not found: $thin_svg" >&2
    exit 66
fi

output_dir=$(dirname -- "$output_ico")
output_file=$(basename -- "$output_ico")
output_stem=${output_file%.*}
png_dir="${output_dir}/${output_stem}-png"

mkdir -p -- "$output_dir" "$png_dir"

low_res_sizes=(16 24 32 48 64)
ico_high_res_sizes=(128 256)
qt_high_res_sizes=(128 256 512)

ico_pngs=()

for size in "${low_res_sizes[@]}"; do
    png="${png_dir}/${output_stem}-${size}.png"
    render_png "$thick_svg" "$size" "$png"
    ico_pngs+=("$png")
done

for size in "${ico_high_res_sizes[@]}"; do
    png="${png_dir}/${output_stem}-${size}.png"
    render_png "$thin_svg" "$size" "$png"
    ico_pngs+=("$png")
done

for size in "${qt_high_res_sizes[@]}"; do
    png="${png_dir}/${output_stem}-${size}.png"
    if [[ ! -f "$png" ]]; then
        render_png "$thin_svg" "$size" "$png"
    fi
done

icotool --create --output "$output_ico" "${ico_pngs[@]}"

cat <<EOF
Created:
  $output_ico
  $png_dir
EOF
