#!/bin/bash

cat <<'EOF'
  _      ______  _______ _____     _____ ____  __  __ _____
 | |    |  ____|/ ____| |  __ \   / ____/ __ \|  \/  |  __ \
 | |    | |__  | (___   | |  | | | |   | |  | | \  / | |__) |
 | |    |  __|  \___ \  | |  | | | |   | |  | | |\/| |  ___/
 | |____| |____ ____) | | |__| | | |___| |__| | |  | | |
 |______|______|_____/  |_____/   \_____\____/|_|  |_|_|

 ESP32 WPSD Companion — Installer
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

# The ESP32 firmware discovers the server via mDNS querying "pi-star.local".
# We register that name as an avahi alias so discovery works regardless of
# whatever hostname this WPSD device is currently set to.
AVAHI_ALIAS="pi-star"
AVAHI_CONF="/etc/avahi/avahi-daemon.conf"

TOTAL_STEPS=8

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
ok "mDNS alias to register: ${AVAHI_ALIAS}.local"
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

sudo apt-get update -qq || warn "apt-get update failed — continuing anyway"

if ! command -v pip3 >/dev/null 2>&1; then
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
# [6/8] Open firewall port for WebSocket
# =========================================================
step 6 "Opening firewall port ${WS_PORT}..."

if sudo iptables -C INPUT -p tcp --dport "${WS_PORT}" -j ACCEPT 2>/dev/null; then
    ok "iptables rule already present"
else
    sudo iptables -I INPUT 1 -p tcp --dport "${WS_PORT}" -j ACCEPT
    ok "iptables rule added for port ${WS_PORT}"
fi

# Persist iptables rules across reboots if iptables-persistent is available
if command -v netfilter-persistent >/dev/null 2>&1; then
    sudo netfilter-persistent save >/dev/null 2>&1 && ok "iptables rules persisted" || warn "could not persist iptables rules"
elif command -v iptables-save >/dev/null 2>&1 && [ -d /etc/iptables ]; then
    sudo iptables-save | sudo tee /etc/iptables/rules.v4 >/dev/null && ok "iptables rules saved to /etc/iptables/rules.v4" || warn "could not save iptables rules"
else
    warn "could not persist iptables rules across reboots — rule is active for this session only"
fi

# =========================================================
# [7/8] Register pi-star.local avahi alias
# =========================================================
step 7 "Registering ${AVAHI_ALIAS}.local mDNS alias..."

# The ESP32 firmware queries mDNS for "pi-star.local" to discover this server.
# We add it as a hostname alias in avahi so the ESP32 finds us regardless of
# whatever hostname this device has been given.

if [ ! -f "${AVAHI_CONF}" ]; then
    warn "${AVAHI_CONF} not found — skipping avahi alias (ESP32 mDNS discovery will fall back to network scan)"
else
    # Check if host-name-aliases already contains our alias
    if grep -qE "^host-name-aliases\s*=.*\b${AVAHI_ALIAS}\b" "${AVAHI_CONF}" 2>/dev/null; then
        ok "${AVAHI_ALIAS}.local alias already present in avahi config"
    else
        # If there's already a host-name-aliases line, append to it; otherwise add new line
        if grep -qE "^host-name-aliases\s*=" "${AVAHI_CONF}"; then
            sudo sed -i -E "s/^(host-name-aliases\s*=.*)/\1,${AVAHI_ALIAS}/" "${AVAHI_CONF}"
            ok "appended ${AVAHI_ALIAS} to existing host-name-aliases"
        else
            # Insert after [server] section header
            sudo sed -i "/^\[server\]/a host-name-aliases=${AVAHI_ALIAS}" "${AVAHI_CONF}"
            ok "added host-name-aliases=${AVAHI_ALIAS} under [server] in avahi config"
        fi
        sudo systemctl restart avahi-daemon || warn "could not restart avahi-daemon"
        ok "avahi-daemon restarted"
    fi
fi

# =========================================================
# [8/8] Install and start systemd service
# =========================================================
step 8 "Installing systemd service..."

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

if [ -f "${AVAHI_CONF}" ] && grep -qE "\b${AVAHI_ALIAS}\b" "${AVAHI_CONF}"; then
    ok "${AVAHI_ALIAS}.local mDNS alias is configured"
else
    warn "${AVAHI_ALIAS}.local mDNS alias is NOT configured"
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
echo " mDNS alias : ${AVAHI_ALIAS}.local  (-> $(hostname).local)"
echo
echo " Useful commands:"
echo "   sudo systemctl status ${SERVICE_NAME} --no-pager"
echo "   journalctl -u ${SERVICE_NAME} -f"
echo "   python3 ${PY_SCRIPT}"
echo "========================================================="
