# ESP32 WPSD Companion

A real-time DMR hotspot monitor built on an **ESP32 CYD**
(Cheap Yellow Display — 320×240 TFT with touchscreen).

It connects via WebSocket to a lightweight Python server running on your **WPSD hotspot** and displays live DMR activity, last-heard stations, static talkgroups, and hotspot information — all on a standalone touchscreen device.

---

# Features

- Live DMR activity
  - Callsign
  - Name
  - City / state / country
  - Country flag
  - Talkgroup
  - Slot
  - BER
  - RSSI
  - Packet loss
  - Duration
  - Live RX / TX / idle status

- Last heard list
  - Recent stations with callsign, name, flag, talkgroup and local time
  - Separate duration / packet-loss view

- Static talkgroups
  - Loaded from the WPSD server with resolved names
  - Scrollable on the touchscreen when more entries are configured

- Hotspot information page
  - Callsign and DMR ID
  - Operator
  - QTH
  - RX frequency
  - TX frequency
  - Colour code
  - Service status
  - System uptime

- Touch navigation
  - Tap screen to cycle through pages
  - On the Static TG page, tap the bottom bar to scroll up/down

- Automatic hotspot discovery
  - Last working IP address saved in NVS
  - mDNS (`wpsd.local`)
  - Automatic LAN scan fallback
  - On-screen discovery status and retry countdown

- Captive portal Wi-Fi setup
  - Automatic Wi-Fi configuration portal
  - Credentials saved in flash memory
  - QR code shown on the CYD screen for quick phone setup

- Clock synchronization
  - Time and UTC offset initialized from WPSD

- Automatic OTA firmware updates
  - Checks GitHub for a newer build on every boot
  - Updates app firmware automatically if a new version is available
  - Updates SPIFFS only when the filesystem image checksum changed
  - No action required — the device updates itself silently

- Offline/reconnect handling
  - Shows a hotspot offline screen when the WebSocket disconnects
  - Reconnects Wi-Fi and WebSocket automatically

---

# Display Pages

| Page | Description |
|------|-------------|
| 0 — Live | Real-time QSO details + recent activity |
| 1 — Last Heard | Recent stations, talkgroup and local time |
| 2 — Last Heard DUR/LOSS | Recent stations with last duration and packet loss |
| 3 — Static TGs | Configured talkgroups with names; bottom bar scrolls when needed |
| 4 — Hotspot Info | Callsign, DMR ID, operator, QTH, RF settings, service and uptime |

---

# Architecture

```text
WPSD (Raspberry Pi)
  └── monitor_mmdvm_ws.py
        ├── Reads MMDVM logs (live DMR events)
        ├── Parses hotspot configuration
        ├── Loads DMR ID database
        ├── Fetches BrandMeister talkgroups
        └── Sends JSON via WebSocket (port 8765)

ESP32 CYD
  └── Firmware (PlatformIO)
        ├── Connects to WebSocket server
        ├── Parses JSON messages
        ├── Auto-discovers hotspot (NVS cache → mDNS → LAN scan)
        ├── Checks GitHub firmware manifest for OTA updates
        └── Renders UI on TFT display
```

---

# JSON Message Types

- `snapshot`
  - Configuration, service status, RadioID metadata, station identity, static talkgroups, server time and UTC offset

- `live`
  - Real-time DMR activity, callsign lookup, talkgroup, BER, RSSI, packet loss and duration

- `heard_summary`
  - Recent stations with last heard time, last talkgroup, last duration and last packet loss

---

# Hardware

| Component | Details |
|----------|---------|
| ESP32 | ESP32 Dev Module |
| Display | 2.8" ILI9341 TFT (320×240) |
| Touch | XPT2046 |
| Hotspot | Raspberry Pi running WPSD |

Target device:

**ESP32-2432S028**
("Cheap Yellow Display")

More information:

https://randomnerdtutorials.com/cheap-yellow-display-esp32-2432s028r/

---

# Required Filesystem Assets

The firmware expects image assets stored in the SPIFFS/LittleFS filesystem.

Examples:

```text
/splash_screen.jpg
/timer.jpg
/flags/large/*.jpg
/flags/small/*.jpg
```

Make sure the filesystem image is uploaded/flashed together with the firmware.

Without these files some UI elements or flags may be missing.

---

# Installation

# 1 — Flash the ESP32

You can either:

- Build using PlatformIO (VS Code)
or
- Use the web installer:

https://wpsd.myshack.ch/

---

# 2 — Configure Wi-Fi

On first boot the ESP32 automatically creates a Wi-Fi configuration portal.

Connect your phone or computer to:

```text
WPSD-Setup
```

The CYD screen also shows a QR code for this open setup network.

Then open the captive portal page and enter your Wi-Fi credentials.

Credentials are stored in flash memory.

The ESP32 uses `esp32-wpsd` as its hostname after Wi-Fi is configured.

---

# Factory Reset

To erase saved Wi-Fi settings:

- Touch and hold the screen during boot
- Keep pressed for about 3 seconds

The device will erase stored credentials and restart configuration mode.

If saved credentials exist but Wi-Fi cannot connect, the firmware keeps retrying. You can also hold the touchscreen for about 3 seconds on the error/retry screen to erase saved credentials and return to setup mode.

---

# Automatic Firmware Updates (OTA)

On every boot, the ESP32 automatically checks GitHub for a newer firmware version.

If a newer build is available:

1. The display shows a **FIRMWARE UPDATE** screen
2. SPIFFS filesystem is updated first, but only when its SHA-256 checksum changed
3. App firmware is flashed
4. Device reboots into the new firmware

No action is required from the user.

If the manifest cannot be reached, the device continues booting and shows an update-check-failed banner with the current build number.

The update screen shows:
- Current build number
- New build number
- Progress bar and percentage for each stage

Do not power off the device during an update.

---

# 3 — Connect to WPSD via SSH

Many users are not familiar with SSH access to WPSD.

This section explains how to connect from Windows, macOS or Linux.

---

## Step 1 — Find your hotspot IP address

Possible methods:

- WPSD dashboard
- Router DHCP client list
- Using hostname:

```text
wpsd.local
```

Examples:

```text
192.168.1.42
```

or

```text
wpsd.local
```

---

## Step 2 — Open an SSH terminal

### Windows

You can use:

- PowerShell (recommended)
or
- PuTTY

Example:

```powershell
ssh pi-star@wpsd.local
```

or

```powershell
ssh pi-star@192.168.1.42
```

### macOS / Linux

Open Terminal and type:

```bash
ssh pi-star@wpsd.local
```

---

## Step 3 — Accept SSH fingerprint

The first connection may display:

```text
The authenticity of host can't be established
```

This is normal.

Type:

```text
yes
```

and press Enter.

---

## Step 4 — Enter password

Enter your WPSD password.

Important:
the password will not visually appear while typing.
This is normal Linux behaviour.

---

## Step 5 — Run installer

Once connected via SSH, run:

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/HB9IIU/ESP32-WPSD-COMPANION/main/InstallationFiles/install_all.sh)
```

The installer automatically:

- installs dependencies
- installs WebSocket server
- configures firewall
- configures Avahi/mDNS
- creates systemd service

---

# Updating Existing Installation

To update only the Python server script:

```bash
curl -fL "https://raw.githubusercontent.com/HB9IIU/ESP32-WPSD-COMPANION/main/InstallationFiles/monitor_mmdvm_ws.py" \
  -o /home/pi-star/monitor_mmdvm_ws.py \
  && sudo systemctl restart monitor_mmdvm_ws.service
```

---

# Service Management

Useful commands:

```bash
sudo systemctl status monitor_mmdvm_ws.service
sudo systemctl restart monitor_mmdvm_ws.service
sudo journalctl -u monitor_mmdvm_ws.service -f
```

---

# Troubleshooting

## ESP32 cannot connect

Check:

- ESP32 and hotspot are on same network
- Wi-Fi credentials are correct
- WebSocket service is running
- Port 8765 is reachable

---

## `wpsd.local` does not work

mDNS sometimes does not work correctly on Windows.

Use direct IP address instead.

Example:

```text
192.168.1.42
```

---

## SSH connection refused

Check:

- SSH enabled in WPSD
- Correct IP address
- Hotspot powered on

---

## Missing flags or splash screen

Make sure SPIFFS/LittleFS image was uploaded correctly.

---

# Dependencies

## ESP32 firmware

- TFT_eSPI
- XPT2046_Touchscreen
- arduinoWebSockets
- ArduinoJson v7
- espressif32 @ 6.9.0

---

## WPSD server

- Python 3.9+
- websockets==13.1

---

# License

MIT License

---

# Author

HB9IIU
