#!/bin/zsh
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${1:-$repo_root/build/dev}"
out_dir="${2:-/tmp/neuracoust-video-interchange-qa}"
fixture="$build_dir/neuracoust_interchange_roundtrip_fixture"

if [[ ! -x "$fixture" ]]; then
  echo "Missing fixture tool: $fixture" >&2
  exit 2
fi

rm -rf "$out_dir"
mkdir -p "$out_dir"
"$fixture" "$out_dir" >/tmp/neuracoust-video-interchange-qa.log

/usr/bin/xmllint --noout "$out_dir/Neuracoust External Roundtrip.fcpxml"
/usr/bin/xmllint --noout "$out_dir/Neuracoust External Roundtrip Resolve.fcpxml"
/usr/bin/grep -q 'com.neuracoust.daw.interchangeProfile" value="DaVinci Resolve"' "$out_dir/Neuracoust External Roundtrip Resolve.fcpxml"
/usr/bin/grep -q '* INTERCHANGE_PROFILE: DaVinci Resolve' "$out_dir/Neuracoust External Roundtrip Resolve.edl"
/usr/bin/grep -q '* INTERCHANGE_PROFILE: Final Cut Pro' "$out_dir/Neuracoust External Roundtrip.edl"

final_cut="$(/usr/bin/find /Applications -maxdepth 1 -iname '*Final*Cut*.app' -print -quit)"
resolve="$(/usr/bin/find /Applications -maxdepth 2 -iname '*Resolve*.app' -print -quit)"

echo "interchangeQa=passed"
echo "fixtureDir=$out_dir"
echo "finalCut=${final_cut:-not-installed}"
echo "resolve=${resolve:-not-installed}"
