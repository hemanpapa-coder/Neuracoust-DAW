#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SDK_DIR="$ROOT_DIR/third_party/vst3sdk"

mkdir -p "$ROOT_DIR/third_party"
if [[ ! -d "$SDK_DIR/.git" ]]; then
  git clone --depth 1 https://github.com/steinbergmedia/vst3sdk.git "$SDK_DIR"
fi

git -C "$SDK_DIR" submodule update --init --depth 1 pluginterfaces base public.sdk cmake
echo "$SDK_DIR"
