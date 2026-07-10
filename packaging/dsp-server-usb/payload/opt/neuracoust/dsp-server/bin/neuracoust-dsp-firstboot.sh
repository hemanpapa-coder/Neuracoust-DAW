#!/usr/bin/env bash
set -euo pipefail

CONF_PATH="${NEURACOUST_INSTALL_CONF:-/etc/neuracoust-dsp-server/install.conf}"
if [[ -f "$CONF_PATH" ]]; then
  # shellcheck disable=SC1090
  source "$CONF_PATH"
fi

BIN_DIR="/opt/neuracoust/dsp-server/bin"
REPORT_DIR="${NEURACOUST_REPORT_DIR:-/var/log/neuracoust/dsp-server}"
mkdir -p "$REPORT_DIR"

"$BIN_DIR/neuracoust-dsp-hardware-probe.sh"

if [[ "${AUTO_INSTALL_TO_INTERNAL_SSD:-1}" == "1" && "${INSTALLED_TO_INTERNAL_SSD:-0}" != "1" ]]; then
  "$BIN_DIR/neuracoust-dsp-install-to-ssd.sh"
else
  echo "Skipping internal SSD install."
fi

if command -v systemctl >/dev/null 2>&1; then
  systemctl enable neuracoust-cpu-performance.service >/dev/null 2>&1 || true
  systemctl enable neuracoust-net-rt.service >/dev/null 2>&1 || true
  systemctl enable neuracoust-wol.service >/dev/null 2>&1 || true
  systemctl enable neuracoust-rt-dsp.service >/dev/null 2>&1 || true
  systemctl enable neuracoust-dsp-server.service >/dev/null 2>&1 || true
  systemctl enable --now neuracoust-dsp-update.timer >/dev/null 2>&1 || true
  systemctl restart neuracoust-cpu-performance.service >/dev/null 2>&1 || true
  systemctl restart neuracoust-net-rt.service >/dev/null 2>&1 || true
  systemctl restart neuracoust-wol.service >/dev/null 2>&1 || true
  systemctl restart neuracoust-rt-dsp.service >/dev/null 2>&1 || true
fi

cat > "$REPORT_DIR/firstboot-complete.json" <<JSON
{
  "product": "Neuracoust DSP Server",
  "status": "complete",
  "generatedAt": "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
}
JSON
