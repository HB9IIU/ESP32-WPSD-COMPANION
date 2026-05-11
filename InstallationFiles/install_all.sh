#!/bin/bash

cat <<'EOF'

  ╔══════════════════════════════════════════════════════════╗
  ║                                                          ║
  ║    ███████╗███████╗██████╗ ██████╗ ██████╗               ║
  ║    ██╔════╝██╔════╝██╔══██╗╚════██╗╚════██╗              ║
  ║    █████╗  ███████╗██████╔╝ █████╔╝ █████╔╝              ║
  ║    ██╔══╝  ╚════██║██╔═══╝ ██╔═══╝ ╚═══██╗              ║
  ║    ███████╗███████║██║     ███████╗██████╔╝              ║
  ║    ╚══════╝╚══════╝╚═╝     ╚══════╝╚═════╝               ║
  ║                                                          ║
  ║    ESP32 WPSD Companion — WebSocket Bridge Installer     ║
  ║    by HB9IIU                                             ║
  ║                                                          ║
  ╚══════════════════════════════════════════════════════════╝

EOF

set -euo pipefail

# =========================================================
# CONFIGURATION
# =========================================================

GITHUB_RAW="https://raw.githubusercontent.com/HB9IIU/ESP32-WPSD-COMPANION/main/InstallationFiles"
PY_SCRIPT="/home/pi-star/monitor_mmdvm_ws.py"
SERVICE_NAME="monitor_mmdvm_ws.service"
WS_PORT="8765"
PINNED_WEBSOCKETS_VERSION="13.1"

TOTAL_STEPS=7

# =========================================================
# HELPERS
# =========================================================

step() { echo; echo "[$1/${TOTAL_STEPS}] $2"; }
ok()   { echo "      ✔ $1"; }
info() { echo "      ➜ $1"; }
warn() { echo "      ⚠ $1"; }
die()  { echo "      ✘ ERROR: $1"; exit 1; }

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

# =========================================================
# [1/8] Environment checks
# =========================================================
step 1 "Checking environment..."

require_cmd python3
require_cmd curl
require_cmd systemctl
require_cmd iptables
require_cmd avahi-daemon

PYVER="$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}")')"
ok "Python: ${PYVER}"

if [ -f /etc/os-release ]; then
    . /etc/os-release
    ok "OS: ${PRETTY_NAME:-unknown}"
fi

ok "Hostname: $(hostname)"
ok "Target script: ${PY_SCRIPT}"
ok "Service: ${SERVICE_NAME}"
ok "WebSocket port: ${WS_PORT}"

# =========================================================
# [2/8] Ensure filesystem is writable
# =========================================================
step 2 "Ensuring filesystem is read-write..."

if sudo mount -o remount,rw / 2>/dev/null; then
    ok "root filesystem is read-write"
else
    ok "root filesystem already writable (or remount not needed)"
fi

# =========================================================
# [3/8] Install Python prerequisites
# =========================================================
step 3 "Installing Python prerequisites..."

info "Refreshing package lists — this may take a moment..."
sudo apt-get update 2>&1 | tail -1 || warn "apt-get update failed — continuing anyway"
ok "package lists refreshed"

if ! command -v pip3 >/dev/null 2>&1; then
    info "Installing python3-pip..."
    sudo apt-get install -y python3-pip || die "could not install python3-pip"
    ok "python3-pip installed"
else
    ok "python3-pip already present"
fi

# =========================================================
# [4/8] Ensure pinned websockets version
# =========================================================
step 4 "Checking websockets installation..."

CURRENT_WS_VERSION=""
if python3 -c "import websockets" >/dev/null 2>&1; then
    CURRENT_WS_VERSION="$(python3 -c 'import websockets; print(websockets.__version__)' 2>/dev/null || true)"
fi

if [ "${CURRENT_WS_VERSION}" = "${PINNED_WEBSOCKETS_VERSION}" ]; then
    ok "websockets ${PINNED_WEBSOCKETS_VERSION} already installed"
else
    if [ -n "${CURRENT_WS_VERSION}" ]; then
        info "removing incompatible websockets version: ${CURRENT_WS_VERSION}"
        sudo python3 -m pip uninstall -y --break-system-packages websockets >/dev/null 2>&1 || true
    fi
    sudo python3 -m pip install --no-cache-dir --break-system-packages "websockets==${PINNED_WEBSOCKETS_VERSION}" \
        || die "failed to install websockets ${PINNED_WEBSOCKETS_VERSION}"
    ok "websockets ${PINNED_WEBSOCKETS_VERSION} installed"
fi

WS_VER="$(python3 -c 'import websockets; print(websockets.__version__)')"
[ "${WS_VER}" = "${PINNED_WEBSOCKETS_VERSION}" ] \
    || die "unexpected websockets version after install: ${WS_VER}"
ok "verified websockets: ${WS_VER}"

# =========================================================
# [5/8] Download monitor script
# =========================================================
step 5 "Downloading monitor_mmdvm_ws.py..."

sudo mkdir -p "$(dirname "${PY_SCRIPT}")"
curl -fsSL "${GITHUB_RAW}/monitor_mmdvm_ws.py" -o /tmp/monitor_mmdvm_ws.py \
    || die "failed to download monitor_mmdvm_ws.py"
sudo mv /tmp/monitor_mmdvm_ws.py "${PY_SCRIPT}"
sudo chmod 755 "${PY_SCRIPT}"
ok "script installed to ${PY_SCRIPT}"

# Stop any running instance before reconfiguring
if pgrep -f "python3 ${PY_SCRIPT}" >/dev/null 2>&1; then
    sudo pkill -f "python3 ${PY_SCRIPT}" || true
    ok "stopped running instance"
fi

# =========================================================
# [6/7] Check firewall for WebSocket port
# =========================================================
step 6 "Checking firewall for port ${WS_PORT}..."

DEFAULT_POLICY="$(sudo iptables -L INPUT --line-numbers -n 2>/dev/null | grep '^Chain INPUT' | grep -oP 'policy \K\w+' || echo 'UNKNOWN')"

if [ "${DEFAULT_POLICY}" = "ACCEPT" ]; then
    ok "firewall default policy is ACCEPT — port ${WS_PORT} is open, no rule needed"
else
    info "default INPUT policy is ${DEFAULT_POLICY} — adding explicit ACCEPT rule for port ${WS_PORT}"
    if sudo iptables -C INPUT -p tcp --dport "${WS_PORT}" -j ACCEPT 2>/dev/null; then
        ok "iptables rule already present"
    else
        sudo iptables -I INPUT 1 -p tcp --dport "${WS_PORT}" -j ACCEPT
        ok "iptables rule added for port ${WS_PORT}"
    fi
    if command -v netfilter-persistent >/dev/null 2>&1; then
        sudo netfilter-persistent save >/dev/null 2>&1 && ok "iptables rules persisted" || warn "could not persist iptables rules"
    elif [ -d /etc/iptables ]; then
        sudo iptables-save | sudo tee /etc/iptables/rules.v4 >/dev/null && ok "iptables rules saved" || warn "could not persist iptables rules"
    fi
fi

# =========================================================
# [7/7] Install and start systemd service
# =========================================================
step 7 "Installing systemd service..."

SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}"

sudo tee "${SERVICE_FILE}" >/dev/null <<EOF
[Unit]
Description=MMDVM WebSocket Monitor (WPSD)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=$(dirname "${PY_SCRIPT}")
ExecStart=/usr/bin/python3 ${PY_SCRIPT}
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

ok "service file written to ${SERVICE_FILE}"

sudo chmod 644 "${SERVICE_FILE}"
sudo systemctl daemon-reload  || die "systemctl daemon-reload failed"
sudo systemctl enable "${SERVICE_NAME}" || die "failed to enable ${SERVICE_NAME}"
sudo systemctl restart "${SERVICE_NAME}" || die "failed to restart ${SERVICE_NAME}"
ok "service enabled and started"

# =========================================================
# Update /etc/motd
# =========================================================
MOTD_MARKER="# ESP32-WPSD-COMPANION"
MOTD_FILE="/etc/motd"

if grep -q "${MOTD_MARKER}" "${MOTD_FILE}" 2>/dev/null; then
    # Remove previous block so we can rewrite it cleanly on upgrade
    sudo sed -i "/${MOTD_MARKER}/,/^${MOTD_MARKER}-END$/d" "${MOTD_FILE}"
fi

sudo tee -a "${MOTD_FILE}" >/dev/null <<MOTD

${MOTD_MARKER}
─────────────────────────────────────────────────────────────
  ESP32 WPSD Companion  —  by HB9IIU
─────────────────────────────────────────────────────────────
  A WebSocket bridge is running on this device, streaming
  real-time DMR activity to an ESP32 CYD touchscreen display.

  Service : ${SERVICE_NAME}
  Port    : ${WS_PORT}
  Script  : ${PY_SCRIPT}

  Useful commands:
    sudo systemctl status ${SERVICE_NAME} --no-pager
    journalctl -u ${SERVICE_NAME} -f
─────────────────────────────────────────────────────────────
${MOTD_MARKER}-END
MOTD

ok "/etc/motd updated"

# =========================================================
# Final verification
# =========================================================
echo
echo "========================================================="
echo " Verification"
echo "========================================================="

sleep 2

if systemctl is-active --quiet "${SERVICE_NAME}"; then
    ok "service is active"
else
    warn "service is not active — check: journalctl -u ${SERVICE_NAME} -n 50"
fi

if sudo ss -ltnp 2>/dev/null | grep -q ":${WS_PORT} "; then
    ok "port ${WS_PORT} is listening"
else
    warn "port ${WS_PORT} is not yet listening (service may still be starting)"
fi

echo
echo "========================================================="
echo " Installation complete"
echo "========================================================="
echo " Python     : ${PYVER}"
echo " websockets : ${WS_VER}"
echo " Script     : ${PY_SCRIPT}"
echo " Service    : ${SERVICE_NAME}"
echo " Port       : ${WS_PORT}"
echo " WPSD host  : $(hostname).local"
echo
echo " Useful commands:"
echo "   sudo systemctl status ${SERVICE_NAME} --no-pager"
echo "   journalctl -u ${SERVICE_NAME} -f"
echo "   python3 ${PY_SCRIPT}"
echo "========================================================="
