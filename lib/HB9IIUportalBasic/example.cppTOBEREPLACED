// example.cpp
// Demo for HB9IIUportalConfiguratorCYD on CYD ESP32
// - Starts Wi-Fi using saved credentials if available
// - If not, launches captive portal "HB9IIUSetup" with QR code on TFT
// - Supports factory reset via button
// - Shows a simple "Connected" screen on TFT when STA Wi-Fi is active

#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include "HB9IIUportalConfigurator.h"

// ====== PIN CONFIG (ADAPT FOR YOUR CYD BOARD) ======
const uint8_t PIN_FACTORY_BUTTON = 0;   // e.g. BOOT button (active LOW, with INPUT_PULLUP)
const uint8_t PIN_FACTORY_LED    = 2;   // Any GPIO connected to an LED, or unused if you prefer

// ====== GLOBALS ======
TFT_eSPI tft;           // Main app TFT instance (library uses its own internal one for the QR portal)
bool lastConnected = false;

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("===== HB9IIUPortal CYD Demo =====");

    // --- Configure factory reset button + LED ---
    pinMode(PIN_FACTORY_BUTTON, INPUT_PULLUP);
    pinMode(PIN_FACTORY_LED, OUTPUT);
    digitalWrite(PIN_FACTORY_LED, LOW);

    // Check if user wants a factory reset (hold button LOW at boot for ~1s)
    HB9IIUPortal::checkFactoryReset(PIN_FACTORY_BUTTON, PIN_FACTORY_LED);
    // If reset confirmed, this function will erase NVS and restart, so it never returns.

    // --- Init TFT for main application UI (not the portal QR screen) ---
    tft.init();
    tft.setRotation(1); // Adjust orientation for your CYD
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("HB9IIUPortal CYD Demo", tft.width() / 2, tft.height() / 2);

    // --- Start Wi-Fi / Portal handling ---
    // Pass an optional hostname; this becomes the STA hostname and mDNS name (hostname.local)
    HB9IIUPortal::begin("cyd-demo");

    lastConnected = HB9IIUPortal::isConnected();

    if (!lastConnected)
    {
        Serial.println("No working Wi-Fi credentials -> captive portal with QR will start.");
        Serial.println("Connect to 'HB9IIUSetup' and configure Wi-Fi.");
    }
    else
    {
        Serial.println("Connected with saved Wi-Fi credentials.");
    }
}

void loop()
{
    // Let the library handle WebServer + DNS for captive portal
    HB9IIUPortal::loop();

    // Track connection status changes
    bool nowConnected = HB9IIUPortal::isConnected();

    // Transition: Just connected to Wi-Fi
    if (nowConnected && !lastConnected)
    {
        Serial.println("Wi-Fi connection established. Switching to main UI.");

        // Simple connected screen on TFT (main app)
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TL_DATUM);
        tft.setCursor(4, 4);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);

        tft.println("Wi-Fi Connected!");
        tft.println();

        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.print("SSID: ");
        tft.println(WiFi.SSID());

        tft.print("IP:   ");
        tft.println(WiFi.localIP().toString());

        // Optional: flash LED to indicate success
        for (int i = 0; i < 3; ++i)
        {
            digitalWrite(PIN_FACTORY_LED, HIGH);
            delay(150);
            digitalWrite(PIN_FACTORY_LED, LOW);
            delay(150);
        }
    }

    // Transition: Lost connection (optional handling)
    if (!nowConnected && lastConnected)
    {
        Serial.println("Wi-Fi connection lost. Library may start portal again.");
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Wi-Fi lost...", tft.width() / 2, tft.height() / 2);
    }

    lastConnected = nowConnected;

    // If not connected and in AP mode, the library is drawing the QR screen on its
    // own internal TFT_eSPI instance, so we don't touch the TFT here.
    // You could poll isInAPMode() if you want:
    //   if (HB9IIUPortal::isInAPMode()) { ... }

    delay(10);  // keep loop responsive but not too busy
}
