// APmodeWifiConfigExample.cpp__
// Example: Wi-Fi configuration with captive portal and AP mode for CYD ESP32
//
// - Attempts to connect to Wi-Fi using saved credentials (from NVS)
// - If no credentials or connection fails, launches a captive portal (AP mode) with QR code for easy phone connection
// - User can connect to the AP and configure Wi-Fi via web portal
// - Supports factory reset via TFT touch/button at boot (see HB9IIUPortal::checkFactoryReset)
// - After successful Wi-Fi connection, shows a simple status screen on the TFT
// - Main application logic runs only after Wi-Fi is connected
//
// Usage:
//   - Place your main application code in loop() (after Wi-Fi is up)
//   - All Wi-Fi and portal logic is handled in setup(), blocking until ready
//
// Dependencies:
//   - HB9IIUportalBasic library (handles Wi-Fi, AP, portal, QR, etc.)
//   - TFT_eSPI for display
//   - PlatformIO/Arduino framework

#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include "HB9IIUportalBasic.h"
#include <HB9IIU_BacklightControl.h>

// ====== GLOBALS ======
TFT_eSPI tft; // Main app TFT instance (library uses its own internal one for the QR portal)
bool lastConnected = false;

void setup()
{
    Serial.begin(115200);
    delay(1000); // Allow time for Serial to initialize

    // --- Initialize TFT for main application UI (not the portal QR screen) ---
    tft.init();
    tft.setRotation(1); 
    tft.fillScreen(TFT_BLACK);
    tft.invertDisplay(HB9_TFT_INVERT);
    backlightInit();

    // Check for factory reset (hold touch/button at boot to erase all settings)
    HB9IIUPortal::checkFactoryReset();

    // --- Start Wi-Fi / Portal handling ---
    // Attempts to connect to saved Wi-Fi; launches AP/captive portal if needed
    HB9IIUPortal::begin("cyd-demo");

    // Block here until Wi-Fi is connected
    // If in AP mode, process captive portal events (web server, DNS, etc.)
    while (!HB9IIUPortal::isConnected()) {
        if (HB9IIUPortal::isInAPMode()) {
            HB9IIUPortal::loop();
        }
        delay(10); // avoid busy loop
    }

    lastConnected = true;

    // --- Show connected screen ---
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
    // Add any additional startup logic here if needed
}

void loop()
{
    // Main application logic only
    // Wi-Fi is guaranteed to be connected at this point
    // ...add your normal app code here...
    delay(10);
}
