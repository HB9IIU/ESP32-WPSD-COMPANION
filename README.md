# ESP32 Pi-Star Companion

A real-time DMR hotspot monitor built on an **ESP32 CYD**  
(Cheap Yellow Display — 320×240 TFT with touchscreen).

It connects via WebSocket to a lightweight Python server running on your **Pi-Star hotspot** and displays live DMR activity, last-heard stations, static talkgroups, and hotspot information — all on a standalone touchscreen device.

---

## 🚀 Features

- **Live DMR activity**
  - Callsign, name, country flag, talkgroup, slot, BER, RSSI, duration

- **Last heard list**
  - Scrollable list with callsign, name, talkgroup and local time

- **Static talkgroups**
  - Automatically fetched from BrandMeister API (with names)

- **Hotspot information page**
  - Operator, QTH, RX/TX frequencies, colour code, power, network, service status

- **Touch navigation**
  - Tap anywhere to cycle through pages

- **Clock synchronization**
  - Time and UTC offset initialized from Pi-Star

---

## 📺 Display Pages

| Page | Description |
|------|------------|
| **0 — Live** | Real-time QSO details + recent activity |
| **1 — Last Heard** | Last 10 stations |
| **2 — Static TGs** | Configured talkgroups with names |
| **3 — Hotspot Info** | Device and network configuration |

---

## 🧠 Architecture

```raw
Pi-Star (Raspberry Pi)
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
        └── Renders UI on TFT display
```

### JSON message types

- `snapshot` → configuration + static data (on connect / change)
- `live` → real-time DMR activity
- `heard_summary` → last heard stations

---

## 🧰 Hardware

| Component | Details |
|----------|--------|
| ESP32 | ESP32 Dev Module |
| Display | 2.8" ILI9341 TFT (320×240) |
| Touch | XPT2046 |
| Hotspot | Raspberry Pi running Pi-Star |

Target device:  
**ESP32-2432S028 ("Cheap Yellow Display")**

More info:  
https://randomnerdtutorials.com/cheap-yellow-display-esp32-2432s028r/

---

## ⚙️ Installation

### 1. Flash the ESP32

- Use **PlatformIO (VS Code)**  
  or  
- Use the web flasher:  
  👉 https://esp32projects.myshack.ch/

---

### 2. Install the Pi-Star WebSocket server

SSH into your Pi-Star and run the installer (handles everything: dependencies, firewall, systemd service):

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/HB9IIU/ESP32-PI-STAR-COMPANION/main/InstallationFiles/install_all.sh)
```

**Updating an existing installation** (script only, service already installed):

```bash
sudo wget -O /home/pi-star/monitor_mmdvm_ws.py \
https://raw.githubusercontent.com/HB9IIU/ESP32-PI-STAR-COMPANION/main/InstallationFiles/monitor_mmdvm_ws.py \
&& sudo systemctl restart monitor_mmdvm_ws
```

---

## 📦 Dependencies

### ESP32 firmware

- TFT_eSPI
- XPT2046_Touchscreen
- arduinoWebSockets
- ArduinoJson v7
- espressif32 @ 6.9.0

---

### Pi-Star server

- Python 3.9+
- `websockets==13.1`

---

## 🔧 Service Management (Pi-Star)

```bash
sudo systemctl status monitor_mmdvm_ws
sudo systemctl restart monitor_mmdvm_ws
sudo journalctl -u monitor_mmdvm_ws -f
```

---

## 📄 License

MIT License

---

## 📡 Author

HB9IIU

