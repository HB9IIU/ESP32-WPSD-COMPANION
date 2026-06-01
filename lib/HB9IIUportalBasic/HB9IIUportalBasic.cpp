#include "HB9IIUportalBasic.h"

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include "nvs_flash.h"

#include "config_page.h"  // const char index_html[] PROGMEM = "..."
#include "success_page.h" // const char html_success[] PROGMEM = "..."
#include "error_page.h"   // const char html_error[] PROGMEM = "..."

#include <TFT_eSPI.h>
#include <qrcode.h> // ricmoo/QRCode

namespace HB9IIUPortal
{
    // ───────── INTERNAL STATE ─────────
    static const byte DNS_PORT = 53;
    static DNSServer dnsServer;
    static IPAddress apIP(192, 168, 4, 1);

    static WebServer server(80);
    static Preferences prefs;

    static int scanCount = 0;
    static bool inAPmode = false;
    static bool connected = false;
    static bool s_credentialsExist = false; // true when NVS has any saved network (even if connection failed)

    static constexpr int MAX_WIFI_SLOTS = 3;

    // Optional hostname for STA + mDNS
    static String g_hostname;

    // Cached scan result for fast /scan response
    static String g_scanJson = "[]"; // cached JSON array of SSID labels
    static bool g_scanReady = false;

    // ───── CYD TFT + QR support (ONLY difference vs original) ─────
    static TFT_eSPI tft = TFT_eSPI();
    static QRCode qrcode;

    // QR version 6 → plenty for short Wi-Fi string
    constexpr int QR_VERSION = 6;
    constexpr int QR_PIXELS = 4 * QR_VERSION + 17;
    constexpr int QR_BUFFER_LEN = (QR_PIXELS * QR_PIXELS + 7) / 8;
    static uint8_t qrcodeData[QR_BUFFER_LEN];

    static void drawQRCodeCentered(QRCode *qrcode);

    // ───────── INTERNAL PROTOTYPES (original) ─────────
    static bool tryToConnectSavedWiFi();
    static void startConfigurationPortal();
    static void handleRootCaptivePortal();
    static void handleScanCaptivePortal();
    static void handleSaveCaptivePortal();
    static void handleRestartCaptivePortal();
    static void printNetworkInfoAndMDNS();
    static void buildScanResultsCache();
    static bool testWiFiCredentials(const String &ssid, const String &password, uint16_t timeoutMs = 10000);

    // ───────── PUBLIC API ─────────

    void begin(const char *hostname)
    {
        Serial.println(F("[HB9IIUPortal] begin()"));

        // Store hostname (if provided) for STA + mDNS
        g_hostname = "";
        if (hostname != nullptr && hostname[0] != '\0')
        {
            g_hostname = hostname;
        }

        if (tryToConnectSavedWiFi())
        {
            inAPmode = false;
            connected = true;
            Serial.println(F("[HB9IIUPortal] Using saved WiFi, no captive portal needed."));
            printNetworkInfoAndMDNS();
        }
        else if (!s_credentialsExist)
        {
            // No credentials saved at all → start captive portal
            connected = false;
            inAPmode = true;
            g_scanJson = "[]";
            g_scanReady = false;
            startConfigurationPortal();
        }
        else
        {
            // Had credentials but connection failed → caller will retry
            connected = false;
            inAPmode = false;
        }
    }

    void loop()
    {
        server.handleClient();

        if (inAPmode)
        {
            dnsServer.processNextRequest(); // important for captive portal
        }
    }

    bool checkFactoryReset()
    {
        // Not implemented: this project uses XPT2046_Touchscreen (not TFT_eSPI touch).
        // Factory reset is handled in main.cpp before calling begin().
        return false;
    }


    void eraseAllPreferencesAndRestart()
    {
        Serial.println(F("⚠️ [HB9IIUPortal] Erasing all NVS data (wifi, config, iPhonetime, etc.)..."));

        esp_err_t err = nvs_flash_erase();
        if (err == ESP_OK)
        {
            Serial.println(F("✅ [HB9IIUPortal] NVS erased successfully. Restarting..."));
        }
        else
        {
            Serial.printf("❌ [HB9IIUPortal] Failed to erase NVS. Error: %d\n", err);
        }
        ESP.restart();
    }

    bool isInAPMode()
    {
        return inAPmode;
    }

    bool isConnected()
    {
        return connected && (WiFi.status() == WL_CONNECTED);
    }

    // ───────── INTERNAL IMPLEMENTATION ─────────

    static bool tryToConnectSavedWiFi()
    {
        Serial.println("[HB9IIUPortal] Attempting to load saved WiFi credentials...");
        s_credentialsExist = false;

        if (!prefs.begin("wifi", false))
        {
            Serial.println("⚠️ [HB9IIUPortal] Failed to open NVS namespace 'wifi'.");
            return false;
        }

        // Migrate old single-network format (ssid/pass keys, no count key)
        if (prefs.isKey("ssid") && !prefs.isKey("count"))
        {
            Serial.println("[HB9IIUPortal] Migrating old single-network NVS format...");
            String oldSsid = prefs.getString("ssid", "");
            String oldPass = prefs.getString("pass", "");
            prefs.remove("ssid");
            prefs.remove("pass");
            if (!oldSsid.isEmpty())
            {
                prefs.putString("ssid0", oldSsid);
                prefs.putString("pass0", oldPass);
                prefs.putInt("count", 1);
            }
        }

        int count = prefs.getInt("count", 0);
        String ssids[MAX_WIFI_SLOTS];
        String passes[MAX_WIFI_SLOTS];
        for (int i = 0; i < count && i < MAX_WIFI_SLOTS; i++)
        {
            ssids[i] = prefs.getString(("ssid" + String(i)).c_str(), "");
            passes[i] = prefs.getString(("pass" + String(i)).c_str(), "");
        }
        prefs.end();

        if (count == 0)
        {
            Serial.println("⚠️ [HB9IIUPortal] No saved credentials found.");
            return false;
        }

        s_credentialsExist = true;

        WiFiMulti wifiMulti;
        for (int i = 0; i < count && i < MAX_WIFI_SLOTS; i++)
        {
            String ssid = ssids[i];
            // Strip " (-xx dBm)" suffix added by our scan labels (safety)
            int parenIdx = ssid.lastIndexOf('(');
            if (parenIdx > 0 && ssid.endsWith(" dBm)")) { ssid = ssid.substring(0, parenIdx); ssid.trim(); }
            if (ssid.isEmpty()) continue;
            Serial.printf("[HB9IIUPortal] 📡 Registered network [%d]: %s\n", i, ssid.c_str());
            wifiMulti.addAP(ssid.c_str(), passes[i].c_str());
        }

        WiFi.mode(WIFI_STA);
        delay(500);
        if (g_hostname.length()) WiFi.setHostname(g_hostname.c_str());

        Serial.print("[HB9IIUPortal] 🔌 Scanning & connecting to best available network...");

        // wifiMulti.run() scans, picks highest-RSSI known AP, connects with given timeout
        uint8_t wlStatus = wifiMulti.run(15000);

        if (wlStatus != WL_CONNECTED)
        {
            Serial.printf("\n❌ [HB9IIUPortal] Failed to connect. Status: %d\n", wlStatus);
            WiFi.disconnect(true, false);

            tft.setRotation(1);
            tft.fillScreen(TFT_BLACK);
            tft.setTextDatum(MC_DATUM);
            tft.setTextSize(1);
            int xCenter = tft.width() / 2;
            int y2      = tft.height() / 2;
            tft.setTextFont(4);
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("Could not connect to", xCenter, y2 - 40);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawString("any saved network", xCenter, y2);
            tft.setTextFont(2);
            tft.setTextColor(TFT_YELLOW, TFT_BLACK);
            tft.drawString("Retrying...", xCenter, y2 + 30);
            tft.setTextColor(tft.color565(140, 140, 140), TFT_BLACK);
            tft.drawString("Hold touchscreen 3s to factory reset", xCenter, y2 + 55);
            delay(2000);
            return false;
        }

        Serial.printf("\n✅ [HB9IIUPortal] Connected to: %s  IP: %s — probing route...",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

        // Verify the default route is truly installed via TCP probe to 8.8.8.8:53
        bool routeOk = false;
        const IPAddress probeIP(8, 8, 8, 8);
        for (int p = 0; p < 25 && !routeOk; p++)
        {
            WiFiClient c;
            bool ok = c.connect(probeIP, 53, 2000);
            int e = errno;
            c.stop();
            Serial.printf("[probe %d] connected=%d errno=%d\n", p, (int)ok, e);
            routeOk = ok;
            if (!routeOk) delay(100);
        }

        if (!routeOk)
        {
            Serial.println("⚠️ [HB9IIUPortal] Route probe failed — retrying WiFi.");
            WiFi.disconnect(true, false);
            return false;
        }

        Serial.println(" OK");
        return true;
    }

    // NEW: QR drawing helper
    static void drawQRCodeCentered(QRCode *qrcode)
    {
        int qrSize = qrcode->size; // modules per side

        // Choose scale so QR fits nicely on the display
        int maxModulePixelsX = tft.width() / (qrSize + 4);  // + margin
        int maxModulePixelsY = tft.height() / (qrSize + 8); // + margin + text
        int scale = maxModulePixelsX;
        if (maxModulePixelsY < scale)
            scale = maxModulePixelsY;
        if (scale < 2)
            scale = 2; // don't go too tiny

        int qrPixelSize = qrSize * scale;

        // Centered position
        int x0 = (tft.width() - qrPixelSize) / 2;
        int y0 = (tft.height() - qrPixelSize) / 2;

        // White background block around QR
        tft.fillRect(x0 - 4, y0 - 4, qrPixelSize + 8, qrPixelSize + 8, TFT_WHITE);

        // Draw modules
        for (int y = 0; y < qrSize; y++)
        {
            for (int x = 0; x < qrSize; x++)
            {
                bool pixelOn = qrcode_getModule(qrcode, x, y);
                if (pixelOn)
                {
                    // Only draw black modules; white area is already white
                    tft.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, TFT_BLACK);
                }
            }
        }
    }

    static void startConfigurationPortal()
    {
        Serial.println("🌐 [HB9IIUPortal] Starting Wi-Fi configuration portal...");

        // AP + STA so we CAN scan networks
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        WiFi.softAP("WPSD-Setup");

        Serial.print("📶 [HB9IIUPortal] AP IP Address: ");
        Serial.println(WiFi.softAPIP());

        // ─── CYD QR CODE ON TFT (ONLY addition) ───
        tft.setRotation(1); 
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.drawString("WPSD-Setup Wi-Fi", tft.width() / 2, 20);
        tft.drawString("Scan QR to connect", tft.width() / 2, 40);

        // Build Wi-Fi QR payload for OPEN AP:
        //   WIFI:T:nopass;S:WPSD-Setup;;
        String qrPayload = "WIFI:T:nopass;S:WPSD-Setup;;";

        Serial.print("📱 [HB9IIUPortal] QR payload: ");
        Serial.println(qrPayload);

        qrcode_initText(
            &qrcode,
            qrcodeData,
            QR_VERSION,
            ECC_MEDIUM,
            qrPayload.c_str());

        drawQRCodeCentered(&qrcode);

        tft.setTextDatum(BC_DATUM);
        tft.setTextFont(2);
        tft.drawString("Open phone camera and scan",
                       tft.width() / 2,
                       tft.height() - 4);

        // ─── ORIGINAL PORTAL LOGIC BELOW (unchanged) ───

        // Pre-scan once, cache results
        buildScanResultsCache();

        // DNS for captive portal
        dnsServer.start(DNS_PORT, "*", apIP);

        // Main config page + captive URLs
        server.on("/", handleRootCaptivePortal);
        server.on("/generate_204", handleRootCaptivePortal);        // Android
        server.on("/fwlink", handleRootCaptivePortal);              // Windows
        server.on("/hotspot-detect.html", handleRootCaptivePortal); // Apple

        server.on("/scan",    handleScanCaptivePortal);
        server.on("/save",    HTTP_POST, handleSaveCaptivePortal);
        server.on("/restart", handleRestartCaptivePortal);

        server.begin();
        Serial.println("✅ [HB9IIUPortal] Web server started. Connect to 'WPSD-Setup' Wi-Fi.");
 
    }

    static void handleRootCaptivePortal()
    {
        // Serve configuration HTML page
        server.send_P(200, "text/html", index_html);
    }

    // Build and cache JSON array of SSID labels from WiFi.scanNetworks()
    static void buildScanResultsCache()
    {
        Serial.println("🔍 [HB9IIUPortal] Pre-scanning Wi-Fi networks...");
        int n = WiFi.scanNetworks();
        scanCount++;

        Serial.printf("📡 [HB9IIUPortal] Pre-scan #%d: Found %d networks.\n", scanCount, n);

        String json = "[";
        for (int i = 0; i < n; ++i)
        {
            if (i > 0)
                json += ",";

            String label = WiFi.SSID(i);
            int32_t rssi = WiFi.RSSI(i);

            // Build "SSID (-63 dBm)"
            label += " (";
            label += String(rssi);
            label += " dBm)";

            // Minimal JSON string escaping
            label.replace("\\", "\\\\");
            label.replace("\"", "\\\"");

            json += "\"";
            json += label;
            json += "\"";
        }
        json += "]";

        g_scanJson = json;
        g_scanReady = true;
    }

    // /scan now just returns the cached result (fast)
    static void handleScanCaptivePortal()
    {
        Serial.println("📨 [HB9IIUPortal] /scan requested.");

        // If for some reason cache is not ready, build it now
        if (!g_scanReady)
        {
            buildScanResultsCache();
        }

        server.send(200, "application/json", g_scanJson);
    }

    // Test credentials while remaining in AP+STA mode
    static bool testWiFiCredentials(const String &ssid, const String &password, uint16_t timeoutMs)
    {
        Serial.printf("🔐 [HB9IIUPortal] Testing credentials for SSID '%s'...\n", ssid.c_str());

        // Keep AP alive, ensure we are in AP+STA
        WiFi.mode(WIFI_AP_STA);

        // Start STA connection with the new credentials
        WiFi.begin(ssid.c_str(), password.c_str());

        unsigned long start = millis();

        while (millis() - start < timeoutMs)
        {
            wl_status_t st = WiFi.status();
            if (st == WL_CONNECTED)
            {
                Serial.println("✅ [HB9IIUPortal] Test connection successful (credentials OK).");
                return true;
            }

            delay(500);
        }

        Serial.println("❌ [HB9IIUPortal] Test connection failed or timed out. Credentials likely invalid.");
        // Disconnect STA, but keep AP
        WiFi.disconnect(false /*wifioff*/, false /*erase*/);
        return false;
    }

    static void handleSaveCaptivePortal()
    {
        Serial.println("💾 [HB9IIUPortal] Processing Wi-Fi credentials save request...");

        String ssidLabel = server.arg("ssid");
        String password  = server.arg("password");
        String timeStr   = server.hasArg("time") ? server.arg("time") : "";

        // Strip " (… dBm)" to get the real SSID from the scan label
        String ssid = ssidLabel;
        int parenIndex = ssid.indexOf(" (");
        if (parenIndex > 0) { ssid = ssid.substring(0, parenIndex); ssid.trim(); }

        Serial.printf("📝 [HB9IIUPortal] SSID: '%s'  Password: '%s'\n", ssid.c_str(), password.c_str());

        // 1) Test credentials before saving
        if (!testWiFiCredentials(ssid, password, 10000))
        {
            Serial.println("❌ [HB9IIUPortal] Credentials do not work. Staying in portal.");
            server.send(200, "text/html", html_error);
            return;
        }

        // 2) Save to the next available slot (wraps at MAX_WIFI_SLOTS)
        if (prefs.begin("wifi", false))
        {
            int currentCount = prefs.getInt("count", 0);
            int slot         = currentCount % MAX_WIFI_SLOTS;
            int newCount     = (currentCount < MAX_WIFI_SLOTS) ? currentCount + 1 : MAX_WIFI_SLOTS;

            prefs.putString(("ssid" + String(slot)).c_str(), ssid);
            prefs.putString(("pass" + String(slot)).c_str(), password);
            prefs.putInt("count", newCount);
            prefs.end();

            Serial.printf("✅ [HB9IIUPortal] Network saved to slot %d (total: %d).\n", slot, newCount);
        }
        else
        {
            Serial.println("❌ [HB9IIUPortal] Failed to open NVS namespace 'wifi' for writing.");
        }

        // 3) Save iPhone time if provided
        if (timeStr.length() > 0)
        {
            JsonDocument timeDoc;
            DeserializationError err = deserializeJson(timeDoc, timeStr);
            if (!err)
            {
                const char *isoTime  = timeDoc["iso"];
                int64_t unixMillis   = timeDoc["unix"] | 0;
                int     offsetMins   = timeDoc["offset"] | 0;
                Serial.printf("🕒 Time: %s  unix: %lld  offset: %d\n",
                              isoTime ? isoTime : "(null)", unixMillis, offsetMins);
                if (prefs.begin("iPhonetime", false))
                {
                    prefs.putString("localTime", isoTime ? isoTime : "");
                    prefs.putLong64("unixMillis", unixMillis);
                    prefs.putInt("offsetMinutes", offsetMins);
                    prefs.end();
                }
            }
            else
            {
                if (prefs.begin("iPhonetime", false))
                {
                    prefs.putString("localTime", timeStr);
                    prefs.end();
                }
            }
        }

        // 4) Show success page — user chooses to add another or restart
        server.send_P(200, "text/html", html_success);
    }

    static void handleRestartCaptivePortal()
    {
        server.send(200, "text/plain", "Restarting device...");
        delay(300);
        ESP.restart();
    }

    static void printNetworkInfoAndMDNS()
    {
        IPAddress ip = WiFi.localIP();
        IPAddress gw = WiFi.gatewayIP();
        IPAddress sn = WiFi.subnetMask();
        IPAddress dns1 = WiFi.dnsIP(0);
        IPAddress dns2 = WiFi.dnsIP(1);

        Serial.println();
        Serial.printf("   📍 IP Address : %s\n", ip.toString().c_str());
        Serial.printf("   🚪 Gateway    : %s\n", gw.toString().c_str());
        Serial.printf("   📦 Subnet     : %s\n", sn.toString().c_str());
        Serial.printf("   🟢 DNS 1 (DHCP): %s\n", dns1.toString().c_str());

        IPAddress fallbackDNS2(8, 8, 8, 8);

        if ((uint32_t)dns2 == 0) // 0.0.0.0 -> not provided by DHCP
        {
            Serial.printf("   🔵 DNS 2 (fallback): %s\n", fallbackDNS2.toString().c_str());
        }
        else
        {
            Serial.printf("   🔵 DNS 2 (DHCP)   : %s\n", dns2.toString().c_str());
        }

        if (g_hostname.length())
        {
            if (MDNS.begin(g_hostname.c_str()))
            {
                MDNS.addService("http", "tcp", 80);
                Serial.printf("✅ mDNS ready at http://%s.local\n", g_hostname.c_str());
            }
            else
            {
                Serial.println("⚠️ Failed to start mDNS responder.");
            }
        }

        Serial.println();
    }

} // namespace HB9IIUPortal
