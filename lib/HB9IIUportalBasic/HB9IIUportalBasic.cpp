#include "HB9IIUportalBasic.h"

#include <WiFi.h>
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
    static bool s_credentialsExist = false; // true when NVS has ssid+pass (even if connection failed)

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
        s_credentialsExist = false; // reset on every call

        if (!prefs.begin("wifi", false))
        {
            Serial.println("⚠️ [HB9IIUPortal] Failed to open NVS namespace 'wifi'.");
            return false;
        }

        if (!prefs.isKey("ssid") || !prefs.isKey("pass"))
        {
            Serial.println("⚠️ [HB9IIUPortal] No saved credentials found (keys missing).");
            prefs.end();
            return false;
        }

        String ssid = prefs.getString("ssid", "");
        String pass = prefs.getString("pass", "");
        // testing with wrong SSID
        // ssid = "Kilimangaro";
        prefs.end();

        // If SSID looks like "NAME (-48 dBm)" from our own scan label, strip the suffix (extra safety)
        int parenIndex = ssid.lastIndexOf('(');
        if (parenIndex > 0 && ssid.endsWith(" dBm)"))
        {
            ssid = ssid.substring(0, parenIndex);
            ssid.trim();
        }

        if (ssid.isEmpty() || pass.isEmpty())
        {
            Serial.println("⚠️ [HB9IIUPortal] No saved credentials found (empty values).");
            s_credentialsExist = false;
            return false;
        }

        s_credentialsExist = true; // credentials loaded — even if connection fails, don't start portal

        Serial.printf("[HB9IIUPortal] 📡 Found SSID: %s\n", ssid.c_str());
        Serial.printf("[HB9IIUPortal] 🔐 Found Password: %s\n", pass.c_str());

        Serial.printf("[HB9IIUPortal] 🔌 Connecting to WiFi: %s", ssid.c_str());

        WiFi.mode(WIFI_STA);

        // If a hostname was provided in begin(), apply it before connecting
        if (g_hostname.length())
        {
            WiFi.setHostname(g_hostname.c_str());
        }

        WiFi.begin(ssid.c_str(), pass.c_str());

        // Wait up to ~10s (dots on serial only — TFT already shows main.cpp content)
        for (int i = 0; i < 20; ++i)
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                Serial.println();
                Serial.println("✅ [HB9IIUPortal] Connected to WiFi!");
                return true;
            }
            Serial.print(".");
            delay(300);
        }

        Serial.println("\n❌ [HB9IIUPortal] Failed to connect to saved WiFi.");
        WiFi.disconnect(false, false); // clean up STA state before next retry

        // --- Show failed connection message; caller will retry ---
        tft.setRotation(1);
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextSize(1);

        int xCenter = tft.width() / 2;
        int y1 = tft.height() / 2 - 40;
        int y2 = tft.height() / 2;

        // Line 1: error (Font 4, red)
        tft.setTextFont(4);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Could not connect to", xCenter, y1);

        // Line 2: SSID (Font 4, white)
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(ssid, xCenter, y2);

        // Line 3: retrying (Font 2, yellow)
        tft.setTextFont(2);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString("Retrying...", xCenter, y2 + 30);

        // Line 4: factory reset hint (Font 2, grey)
        tft.setTextColor(tft.color565(140, 140, 140), TFT_BLACK);
        tft.drawString("Hold touchscreen 3s to factory reset", xCenter, y2 + 55);

        delay(2000);
        return false;
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
        WiFi.softAP("PiStar-Setup");

        Serial.print("📶 [HB9IIUPortal] AP IP Address: ");
        Serial.println(WiFi.softAPIP());

        // ─── CYD QR CODE ON TFT (ONLY addition) ───
        tft.setRotation(1); 
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.drawString("PiStar-Setup Wi-Fi", tft.width() / 2, 20);
        tft.drawString("Scan QR to connect", tft.width() / 2, 40);

        // Build Wi-Fi QR payload for OPEN AP:
        //   WIFI:T:nopass;S:PiStar-Setup;;
        String qrPayload = "WIFI:T:nopass;S:PiStar-Setup;;";

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

        server.on("/scan", handleScanCaptivePortal);
        server.on("/save", HTTP_POST, handleSaveCaptivePortal);

        server.begin();
        Serial.println("✅ [HB9IIUPortal] Web server started. Connect to 'PiStar-Setup' Wi-Fi.");
 
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

        // Raw form values
        String ssidLabel = server.arg("ssid");
        String password = server.arg("password");
        String timeStr = server.hasArg("time") ? server.arg("time") : "";

        // Strip " (… dBm)" to get the real SSID from label
        String ssid = ssidLabel;
        int parenIndex = ssid.indexOf(" (");
        if (parenIndex > 0)
        {
            ssid = ssid.substring(0, parenIndex);
            ssid.trim();
        }

        Serial.printf("📝 [HB9IIUPortal] Received SSID label: '%s'\n", ssidLabel.c_str());
        Serial.printf("📝 [HB9IIUPortal] Using raw SSID: '%s'\n", ssid.c_str());
        Serial.printf("📝 [HB9IIUPortal] Received Password: '%s'\n", password.c_str());
        Serial.printf("🕒 [HB9IIUPortal] Received Time: '%s'\n", timeStr.c_str());

        // 1) Test the credentials BEFORE saving/restarting, using the RAW SSID
        if (!testWiFiCredentials(ssid, password, 10000)) // 10s timeout
        {
            Serial.println("❌ [HB9IIUPortal] Entered Wi-Fi credentials do NOT work. Staying in portal.");

            // Show the existing HTML error page
            server.send(200, "text/html", html_error);
            return; // IMPORTANT: do not save or restart
        }

        // If we reach here, credentials worked at least once 👍

        // 2) Save Wi-Fi credentials in prefs namespace "wifi"
        if (prefs.begin("wifi", false))
        {
            prefs.putString("ssid", ssid); // save cleaned SSID
            prefs.putString("pass", password);
            prefs.end();
            Serial.println("✅ [HB9IIUPortal] Wi-Fi credentials saved to NVS (namespace 'wifi').");
        }
        else
        {
            Serial.println("❌ [HB9IIUPortal] Failed to open prefs namespace 'wifi' for writing.");
        }

        // 3) Optional: parse and save iPhone time JSON (if provided)
        if (timeStr.length() > 0)
        {
            JsonDocument timeDoc;
            DeserializationError err = deserializeJson(timeDoc, timeStr);

            if (!err)
            {
                // HTML sends: {iso: isoTime, unix: unixMillis, offset: offsetMinutes}
                const char *isoTime = timeDoc["iso"]; // e.g. "2025-11-22T10:15:30Z"
                int64_t unixMillis = timeDoc["unix"] | 0;
                int offsetMins = timeDoc["offset"] | 0;

                Serial.printf("🕒 Parsed time JSON:\n   isoTime: %s\n   unixMillis: %lld\n   offsetMinutes: %d\n",
                              isoTime ? isoTime : "(null)",
                              unixMillis,
                              offsetMins);

                if (prefs.begin("iPhonetime", false))
                {
                    prefs.putString("localTime", isoTime ? isoTime : "");
                    prefs.putLong64("unixMillis", unixMillis);
                    prefs.putInt("offsetMinutes", offsetMins);
                    prefs.end();

                    Serial.println("✅ [HB9IIUPortal] iPhone time JSON saved to NVS (namespace 'iPhonetime').");
                }
                else
                {
                    Serial.println("❌ [HB9IIUPortal] Failed to open prefs namespace 'iPhonetime' for writing.");
                }
            }
            else
            {
                Serial.println("⚠️ [HB9IIUPortal] Failed to parse time JSON, saving raw string instead.");
                if (prefs.begin("iPhonetime", false))
                {
                    prefs.putString("localTime", timeStr);
                    prefs.end();
                }
            }
        }

        // 4) Show success page and reboot as before
        server.send_P(200, "text/html", html_success);
        delay(500);
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
