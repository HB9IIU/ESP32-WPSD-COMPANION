#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include "HB9IIUportalBasic.h"
#define WS_PORT 8765
#define WS_PATH "/"

// Set to true to skip NVS + mDNS steps and force the network scan (for testing)
constexpr bool kSkipFastDiscovery = false;

// Resolved Pi-Star host — set at boot by discoverPiStar()
static char g_wsHost[40] = "";

// Globals

constexpr size_t MAX_CONFIG_JSON_LENGTH = 8192;
constexpr size_t MAX_RSSI_VALUES = 16;
constexpr size_t MAX_HEARD_RECENT_ITEMS = 10;
constexpr size_t MAX_STATIC_TGS = 20;
constexpr uint32_t METRIC_HOLD_MS = 5000;
constexpr uint32_t WS_RECOVERY_INTERVAL_MS = 5000;
constexpr uint32_t WIFI_RECOVERY_INTERVAL_MS = 10000;

struct SnapshotState
{
    char type[16];
    bool server_time_valid;
    int64_t server_time_unix;
    char server_time_iso[40];
    int server_utc_offset_sec;
    char service_state[16];
    int service_pid;
    char service_active_since[64];
    char config_mtime[32];
    int config_mtime_ago_hours;
    char current_log_file[96];
    char config_json[MAX_CONFIG_JSON_LENGTH];
    size_t config_json_length;
    size_t config_section_count;
    bool config_was_truncated;
    char radioid_csv_file[128];
    bool radioid_csv_exists;
    char radioid_csv_mtime[32];
    int radioid_csv_age_hours;
    bool radioid_csv_is_stale;
    int radioid_entries;
    bool radioid_lookup_loaded;
    char radioid_status[24];
    char radioid_last_refresh_attempt[32];
    char radioid_last_refresh_success[32];
    char radioid_last_refresh_error[160];
    char station_callsign[24];
    int station_match_count;
    char station_id[16];
    char station_name[32];
    char station_surname[32];
    char station_city[48];
    char station_state[48];
    char station_country[32];
    char station_country_code[4];
    char hotspot_dmr_id[16];
    struct { int tg; char name[48]; } static_tgs[MAX_STATIC_TGS];
    int static_tg_count;
    bool valid;
} g_snapshot;

struct LiveState
{
    char type[16];
    int event_id;
    char timestamp[32];
    char mode[16];
    char last_event[32];
    char direction[16];
    bool slot_valid;
    int slot;
    char source[24];
    int source_match_count;
    char source_id[16];
    char source_callsign[24];
    char source_name[32];
    char source_surname[32];
    char source_city[48];
    char source_state[48];
    char source_country[32];
    char source_country_code[4];
    char destination[32];
    char talker_alias[64];
    bool duration_valid;
    float duration_sec;
    bool packet_loss_valid;
    float packet_loss_percent;
    bool ber_valid;
    float ber_percent;
    int rssi_values_dbm[MAX_RSSI_VALUES];
    size_t rssi_count;
    char raw_line[196];
    bool valid;
} g_live;

struct HeardRecentItem
{
    char callsign[24];
    char name[32];
    char country_code[4];
    char last_seen[32];
    int64_t last_seen_unix;
    char last_tg[32];
};

struct HeardSummaryState
{
    char type[20];
    int unique_callsigns_total;
    char last_heard_callsign[24];
    char last_heard_at[32];
    HeardRecentItem recent[MAX_HEARD_RECENT_ITEMS];
    size_t recent_count;
    bool valid;
} g_heardSummary;

WebSocketsClient ws;
bool g_clockInitialized = false;
int g_clockUtcOffsetSec = 0;
char g_lastClockText[6] = "";
char g_lastRxCallsignText[24] = "";
char g_lastRxNameText[32] = "";
char g_lastRxLocationText[96] = "";
char g_lastTalkgroupText[48] = "";
char g_lastRssiMetricText[12] = "";
char g_lastBerMetricText[12] = "";
char g_lastLossMetricText[12] = "";
char g_lastDurationMetricText[8] = "";
uint16_t g_lastDurationMetricColor = 0;
char g_lastActivityBannerText[8] = "";
int g_idleBlinkPhase = 0;
int g_idleBlinkPhasePrev = -1; // -1 = full redraw needed
unsigned long g_idleBlinkMs = 0;
constexpr uint32_t IDLE_BLINK_MS = 200;
char g_lastFooterStatusText[64] = "";
bool g_lastFooterSlotValid = false;
int g_lastFooterSlot = 0;
bool g_lastKnownRssiValid = false;
int g_lastKnownRssiDbm = 0;
bool g_lastKnownBerValid = false;
float g_lastKnownBerPercent = 0.0f;
unsigned long g_lastKnownBerUpdatedMs = 0;
bool g_lastKnownLossValid = false;
float g_lastKnownLossPercent = 0.0f;
unsigned long g_lastKnownLossUpdatedMs = 0;
bool g_durationTimerActive = false;
unsigned long g_durationTimerStartedMs = 0;
bool g_lastKnownDurationValid = false;
float g_lastKnownDurationSec = 0.0f;
unsigned long g_lastKnownDurationUpdatedMs = 0;
unsigned long g_nextWsRecoveryMs = 0;
unsigned long g_nextWiFiRecoveryMs = 0;
bool g_wsConnected = false;
unsigned long g_wsDisconnectedAtMs = 0;
unsigned long g_offlineTimerLastMs = 0;
bool g_pendingOfflineDraw = false;
bool g_pendingReconnectDraw = false;
int g_currentPage = 0;
int g_staticTgScrollOffset = 0;
unsigned long g_page2LastRefreshMs = 0;
unsigned long g_lastTouchMs = 0;
constexpr uint32_t PAGE2_REFRESH_MS = 30000;
constexpr uint32_t TOUCH_DEBOUNCE_MS = 600;

void drawClockDemo(const char *timeText);
void drawActivityStatusBannerDemo(const char *statusText);
void drawFooterStatusDemo(const char *statusText);
void drawFooterStatusText(const char *statusText, uint16_t textColor);
void updateFooterStatusTextDisplay(const char *statusText, uint16_t textColor);
void updateRssiMetricDisplay();
void updateBerMetricDisplay();
void updateLossMetricDisplay();
void updateDurationMetricDisplay();
void updateHotspotCallsignDisplay();
void updateRxCallsignDisplay();
void updateRxNameDisplay();
void updateRxFlagDisplay();
void updateActivityStatusBannerDisplay();
void drawRxNameSeparator();
void updateRxLocationDisplay();
void drawRxLocationSeparator();
void updateTalkgroupDisplay();
void updateRecentHeardFlagsDisplay();
void updateFooterStatusDisplay();
void drawTalkgroupSeparators();
void configureWebSocketClient();
void onWsEvent(WStype_t type, uint8_t *payload, size_t length);
void drawHeardListPage();
void drawStaticTgPage();
void drawHotspotInfoPage();
void drawOfflinePage();
void updateOfflineElapsed();
void switchToPage(int page);

// Helpers

void clearSnapshotState()
{
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    strlcpy(g_snapshot.type, "snapshot", sizeof(g_snapshot.type));
}

void clearLiveState()
{
    memset(&g_live, 0, sizeof(g_live));
    strlcpy(g_live.type, "live", sizeof(g_live.type));
}

void clearHeardSummaryState()
{
    memset(&g_heardSummary, 0, sizeof(g_heardSummary));
    strlcpy(g_heardSummary.type, "heard_summary", sizeof(g_heardSummary.type));
}

bool snapshotConfigValue(const char *sectionName, const char *keyName, char *destination, size_t destinationSize)
{
    if (destinationSize == 0)
    {
        return false;
    }

    destination[0] = '\0';

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, g_snapshot.config_json, g_snapshot.config_json_length);
    if (error)
    {
        return false;
    }

    JsonVariantConst value = doc[sectionName][keyName];
    if (value.isNull())
    {
        return false;
    }

    strlcpy(destination, value.as<const char *>(), destinationSize);
    return true;
}

void copyJsonString(JsonVariantConst value, char *destination, size_t destinationSize)
{
    if (destinationSize == 0)
    {
        return;
    }

    if (value.isNull())
    {
        destination[0] = '\0';
        return;
    }

    String text = value.as<String>();
    strlcpy(destination, text.c_str(), destinationSize);
}

void printDivider(const char *title)
{
    Serial.println();
    Serial.println("========================================");
    Serial.printf("%s\n", title);
    Serial.println("========================================");
}

void printConfigSections(JsonVariantConst configVariant)
{
    Serial.printf("Config Sections: %u\n", static_cast<unsigned>(g_snapshot.config_section_count));
    if (g_snapshot.config_was_truncated)
    {
        Serial.println("WARNING: Config payload exceeded local storage and was truncated.");
    }

    JsonObjectConst configObject = configVariant.as<JsonObjectConst>();
    if (configObject.isNull())
    {
        Serial.println("Config payload unavailable.");
        return;
    }

    for (JsonPairConst sectionPair : configObject)
    {
        Serial.println();
        Serial.printf("[%s]\n", sectionPair.key().c_str());

        JsonObjectConst sectionObject = sectionPair.value().as<JsonObjectConst>();
        if (sectionObject.isNull())
        {
            Serial.println("  <non-object section>");
            continue;
        }

        for (JsonPairConst entryPair : sectionObject)
        {
            String valueText = entryPair.value().as<String>();
            Serial.printf("  %-18s = %s\n", entryPair.key().c_str(), valueText.c_str());
        }
    }
}

void printRssiValues()
{
    Serial.print("RSSI dBm          : ");
    if (g_live.rssi_count == 0)
    {
        Serial.println("n/a");
        return;
    }

    for (size_t index = 0; index < g_live.rssi_count; ++index)
    {
        if (index > 0)
        {
            Serial.print(", ");
        }
        Serial.print(g_live.rssi_values_dbm[index]);
    }
    Serial.println();
}

bool liveEventHasDetailPayload()
{
    return g_live.slot_valid ||
           g_live.source[0] != '\0' ||
           g_live.destination[0] != '\0' ||
           g_live.talker_alias[0] != '\0' ||
           g_live.duration_valid ||
           g_live.packet_loss_valid ||
           g_live.ber_valid ||
           g_live.rssi_count > 0;
}

bool isEmptyTalkerAliasEvent()
{
    return strcmp(g_live.last_event, "talker_alias") == 0 &&
           g_live.talker_alias[0] == '\0' &&
           g_live.source[0] == '\0' &&
           g_live.destination[0] == '\0' &&
           !g_live.duration_valid &&
           !g_live.packet_loss_valid &&
           !g_live.ber_valid &&
           g_live.rssi_count == 0;
}

void printSnapshot(JsonVariantConst configVariant)
{
    printDivider("PI-STAR SNAPSHOT");
    Serial.printf("Type              : %s\n", g_snapshot.type);
    Serial.printf("Server Time Unix  : %s", g_snapshot.server_time_valid ? "" : "n/a");
    if (g_snapshot.server_time_valid)
    {
        Serial.printf("%lld\n", static_cast<long long>(g_snapshot.server_time_unix));
    }
    else
    {
        Serial.println();
    }
    Serial.printf("Server Time ISO   : %s\n", g_snapshot.server_time_iso);
    Serial.printf("UTC Offset Sec    : %d\n", g_snapshot.server_utc_offset_sec);
    Serial.printf("Service State     : %s\n", g_snapshot.service_state);
    Serial.printf("Service PID       : %d\n", g_snapshot.service_pid);
    Serial.printf("Active Since      : %s\n", g_snapshot.service_active_since);
    Serial.printf("Config MTime      : %s\n", g_snapshot.config_mtime);
    Serial.printf("Config Age Hours  : %d\n", g_snapshot.config_mtime_ago_hours);
    Serial.printf("Current Log File  : %s\n", g_snapshot.current_log_file);
    Serial.printf("RadioID CSV File  : %s\n", g_snapshot.radioid_csv_file);
    Serial.printf("RadioID CSV Exists: %s\n", g_snapshot.radioid_csv_exists ? "yes" : "no");
    Serial.printf("RadioID CSV MTime : %s\n", g_snapshot.radioid_csv_mtime);
    Serial.printf("RadioID CSV Age   : %d hours\n", g_snapshot.radioid_csv_age_hours);
    Serial.printf("RadioID CSV Stale : %s\n", g_snapshot.radioid_csv_is_stale ? "yes" : "no");
    Serial.printf("RadioID Entries   : %d\n", g_snapshot.radioid_entries);
    Serial.printf("RadioID Loaded    : %s\n", g_snapshot.radioid_lookup_loaded ? "yes" : "no");
    Serial.printf("RadioID Status    : %s\n", g_snapshot.radioid_status);
    Serial.printf("Last Refresh Try  : %s\n", g_snapshot.radioid_last_refresh_attempt);
    Serial.printf("Last Refresh OK   : %s\n", g_snapshot.radioid_last_refresh_success);
    Serial.printf("Last Refresh Error: %s\n", g_snapshot.radioid_last_refresh_error);
    Serial.printf("Station Callsign  : %s\n", g_snapshot.station_callsign);
    Serial.printf("Station Matches   : %d\n", g_snapshot.station_match_count);
    Serial.printf("Station ID        : %s\n", g_snapshot.station_id);
    Serial.printf("Station Name      : %s\n", g_snapshot.station_name);
    Serial.printf("Station Surname   : %s\n", g_snapshot.station_surname);
    Serial.printf("Station City      : %s\n", g_snapshot.station_city);
    Serial.printf("Station State     : %s\n", g_snapshot.station_state);
    Serial.printf("Station Country   : %s\n", g_snapshot.station_country);
    Serial.printf("Station ISO Code  : %s\n", g_snapshot.station_country_code);
    Serial.printf("Hotspot DMR ID    : %s\n", g_snapshot.hotspot_dmr_id);
    Serial.printf("Static TGs        : %d\n", g_snapshot.static_tg_count);
    for (int i = 0; i < g_snapshot.static_tg_count; ++i)
    {
        Serial.printf("  TG %-6d  %s\n", g_snapshot.static_tgs[i].tg, g_snapshot.static_tgs[i].name);
    }
    printConfigSections(configVariant);
    Serial.println("========================================");
}

void printHeardSummary()
{
    printDivider("HEARD SUMMARY");
    Serial.printf("Type              : %s\n", g_heardSummary.type);
    Serial.printf("Unique Callsigns  : %d\n", g_heardSummary.unique_callsigns_total);
    Serial.printf("Last Heard        : %s\n", g_heardSummary.last_heard_callsign);
    Serial.printf("Last Heard At     : %s\n", g_heardSummary.last_heard_at);
    Serial.printf("Recent Entries    : %u\n", static_cast<unsigned>(g_heardSummary.recent_count));

    for (size_t index = 0; index < g_heardSummary.recent_count; ++index)
    {
        const HeardRecentItem &item = g_heardSummary.recent[index];
        Serial.printf("  %u. %s | %s | %s | %s\n",
                      static_cast<unsigned>(index + 1),
                      item.callsign,
                      item.name,
                      item.country_code,
                      item.last_tg);
        Serial.printf("     Last Seen: %s (unix=%lld)\n", item.last_seen, (long long)item.last_seen_unix);
    }

    Serial.println("========================================");
}

void printLive()
{
    if (isEmptyTalkerAliasEvent())
    {
        Serial.printf("[LIVE] empty talker_alias #%d | slot=%d | raw=%s\n",
                      g_live.event_id,
                      g_live.slot_valid ? g_live.slot : -1,
                      g_live.raw_line[0] != '\0' ? g_live.raw_line : "n/a");
        return;
    }

    if (!liveEventHasDetailPayload())
    {
        Serial.printf("[LIVE] sparse event #%d | %s | mode=%s | raw=%s\n",
                      g_live.event_id,
                      g_live.last_event[0] != '\0' ? g_live.last_event : "unknown",
                      g_live.mode[0] != '\0' ? g_live.mode : "unknown",
                      g_live.raw_line[0] != '\0' ? g_live.raw_line : "n/a");
        return;
    }

    printDivider("PI-STAR LIVE EVENT");
    Serial.printf("Type              : %s\n", g_live.type);
    Serial.printf("Event ID          : %d\n", g_live.event_id);
    Serial.printf("Timestamp         : %s\n", g_live.timestamp);
    Serial.printf("Mode              : %s\n", g_live.mode);
    Serial.printf("Last Event        : %s\n", g_live.last_event);
    Serial.printf("Direction         : %s\n", g_live.direction);
    Serial.printf("Slot              : %s", g_live.slot_valid ? "" : "n/a");
    if (g_live.slot_valid)
    {
        Serial.printf("%d\n", g_live.slot);
    }
    else
    {
        Serial.println();
    }
    Serial.printf("Source            : %s\n", g_live.source);
    Serial.printf("Source Matches    : %d\n", g_live.source_match_count);
    Serial.printf("Source ID         : %s\n", g_live.source_id);
    Serial.printf("Source Callsign   : %s\n", g_live.source_callsign);
    Serial.printf("Source Name       : %s\n", g_live.source_name);
    Serial.printf("Source Surname    : %s\n", g_live.source_surname);
    Serial.printf("Source City       : %s\n", g_live.source_city);
    Serial.printf("Source State      : %s\n", g_live.source_state);
    Serial.printf("Source Country    : %s\n", g_live.source_country);
    Serial.printf("Source ISO Code   : %s\n", g_live.source_country_code);
    Serial.printf("Destination       : %s\n", g_live.destination);
    Serial.printf("Talker Alias      : %s\n", g_live.talker_alias);
    Serial.printf("Duration Sec      : %s", g_live.duration_valid ? "" : "n/a");
    if (g_live.duration_valid)
    {
        Serial.printf("%.2f\n", g_live.duration_sec);
    }
    else
    {
        Serial.println();
    }
    Serial.printf("Packet Loss Pct   : %s", g_live.packet_loss_valid ? "" : "n/a");
    if (g_live.packet_loss_valid)
    {
        Serial.printf("%.2f\n", g_live.packet_loss_percent);
    }
    else
    {
        Serial.println();
    }
    Serial.printf("BER Percent       : %s", g_live.ber_valid ? "" : "n/a");
    if (g_live.ber_valid)
    {
        Serial.printf("%.2f\n", g_live.ber_percent);
    }
    else
    {
        Serial.println();
    }
    printRssiValues();
    Serial.printf("Raw Line          : %s\n", g_live.raw_line);
    Serial.println("========================================");
}

void storeConfigJson(JsonVariantConst configVariant)
{
    JsonObjectConst configObject = configVariant.as<JsonObjectConst>();
    if (configObject.isNull())
    {
        g_snapshot.config_json[0] = '\0';
        g_snapshot.config_json_length = 0;
        g_snapshot.config_section_count = 0;
        g_snapshot.config_was_truncated = false;
        return;
    }

    g_snapshot.config_section_count = configObject.size();
    const size_t requiredLength = measureJson(configVariant);
    g_snapshot.config_was_truncated = requiredLength >= sizeof(g_snapshot.config_json);
    g_snapshot.config_json_length = serializeJson(configVariant, g_snapshot.config_json, sizeof(g_snapshot.config_json));
}

void initializeClockFromSnapshot()
{
    if (g_clockInitialized || !g_snapshot.server_time_valid)
    {
        return;
    }

    timeval timeValue = {};
    timeValue.tv_sec = static_cast<time_t>(g_snapshot.server_time_unix);

    if (settimeofday(&timeValue, nullptr) != 0)
    {
        Serial.println("[TIME] Failed to initialize clock from snapshot");
        return;
    }

    g_clockUtcOffsetSec = g_snapshot.server_utc_offset_sec;
    g_clockInitialized = true;
    g_lastClockText[0] = '\0';
    Serial.printf("[TIME] Clock initialized from snapshot: %lld (%s)\n",
                  static_cast<long long>(g_snapshot.server_time_unix),
                  g_snapshot.server_time_iso);
}

void buildClockText(char *destination, size_t destinationSize)
{
    if (destinationSize == 0)
    {
        return;
    }

    if (!g_clockInitialized)
    {
        strlcpy(destination, "--:--", destinationSize);
        return;
    }

    const time_t nowUtc = time(nullptr);
    if (nowUtc <= 0)
    {
        strlcpy(destination, "--:--", destinationSize);
        return;
    }

    const time_t nowLocal = nowUtc + g_clockUtcOffsetSec;
    struct tm timeInfo = {};
    gmtime_r(&nowLocal, &timeInfo);

    if (strftime(destination, destinationSize, "%H:%M", &timeInfo) == 0)
    {
        strlcpy(destination, "--:--", destinationSize);
    }
}

void buildLastSeenTimeText(int64_t lastSeenUnix, char *dest, size_t destSize)
{
    if (destSize == 0)
    {
        return;
    }
    if (lastSeenUnix <= 0)
    {
        strlcpy(dest, "--:--", destSize);
        return;
    }
    const time_t localTime = (time_t)(lastSeenUnix + g_clockUtcOffsetSec);
    struct tm timeInfo = {};
    gmtime_r(&localTime, &timeInfo);
    if (strftime(dest, destSize, "%H:%M", &timeInfo) == 0)
    {
        strlcpy(dest, "--:--", destSize);
    }
}

void updateClockDisplay(bool force = false)
{
    char clockText[6];
    buildClockText(clockText, sizeof(clockText));

    if (!force && strcmp(clockText, g_lastClockText) == 0)
    {
        return;
    }

    drawClockDemo(clockText);
    strlcpy(g_lastClockText, clockText, sizeof(g_lastClockText));
}

// Parsers

void parseSnapshot(JsonDocument &doc)
{
    clearSnapshotState();

    copyJsonString(doc["type"], g_snapshot.type, sizeof(g_snapshot.type));
    g_snapshot.server_time_valid = !doc["server_time_unix"].isNull();
    g_snapshot.server_time_unix = g_snapshot.server_time_valid ? doc["server_time_unix"].as<int64_t>() : 0;
    copyJsonString(doc["server_time_iso"], g_snapshot.server_time_iso, sizeof(g_snapshot.server_time_iso));
    g_snapshot.server_utc_offset_sec = doc["server_utc_offset_sec"] | 0;
    copyJsonString(doc["service"]["state"], g_snapshot.service_state, sizeof(g_snapshot.service_state));
    g_snapshot.service_pid = doc["service"]["main_pid"] | 0;
    copyJsonString(doc["service"]["active_since"], g_snapshot.service_active_since, sizeof(g_snapshot.service_active_since));
    copyJsonString(doc["config_mtime"], g_snapshot.config_mtime, sizeof(g_snapshot.config_mtime));
    g_snapshot.config_mtime_ago_hours = doc["config_mtime_ago_hours"] | 0;
    copyJsonString(doc["current_log_file"], g_snapshot.current_log_file, sizeof(g_snapshot.current_log_file));
    storeConfigJson(doc["config"]);
    copyJsonString(doc["radioid_csv_file"], g_snapshot.radioid_csv_file, sizeof(g_snapshot.radioid_csv_file));
    g_snapshot.radioid_csv_exists = doc["radioid_csv_exists"] | false;
    copyJsonString(doc["radioid_csv_mtime"], g_snapshot.radioid_csv_mtime, sizeof(g_snapshot.radioid_csv_mtime));
    g_snapshot.radioid_csv_age_hours = doc["radioid_csv_age_hours"] | 0;
    g_snapshot.radioid_csv_is_stale = doc["radioid_csv_is_stale"] | true;
    g_snapshot.radioid_entries = doc["radioid_entries"] | 0;
    g_snapshot.radioid_lookup_loaded = doc["radioid_lookup_loaded"] | false;
    copyJsonString(doc["radioid_status"], g_snapshot.radioid_status, sizeof(g_snapshot.radioid_status));
    copyJsonString(doc["radioid_last_refresh_attempt"], g_snapshot.radioid_last_refresh_attempt, sizeof(g_snapshot.radioid_last_refresh_attempt));
    copyJsonString(doc["radioid_last_refresh_success"], g_snapshot.radioid_last_refresh_success, sizeof(g_snapshot.radioid_last_refresh_success));
    copyJsonString(doc["radioid_last_refresh_error"], g_snapshot.radioid_last_refresh_error, sizeof(g_snapshot.radioid_last_refresh_error));
    copyJsonString(doc["station_callsign"], g_snapshot.station_callsign, sizeof(g_snapshot.station_callsign));
    g_snapshot.station_match_count = doc["station_match_count"] | 0;
    copyJsonString(doc["station_id"], g_snapshot.station_id, sizeof(g_snapshot.station_id));
    copyJsonString(doc["station_name"], g_snapshot.station_name, sizeof(g_snapshot.station_name));
    copyJsonString(doc["station_surname"], g_snapshot.station_surname, sizeof(g_snapshot.station_surname));
    copyJsonString(doc["station_city"], g_snapshot.station_city, sizeof(g_snapshot.station_city));
    copyJsonString(doc["station_state"], g_snapshot.station_state, sizeof(g_snapshot.station_state));
    copyJsonString(doc["station_country"], g_snapshot.station_country, sizeof(g_snapshot.station_country));
    copyJsonString(doc["station_country_code"], g_snapshot.station_country_code, sizeof(g_snapshot.station_country_code));
    copyJsonString(doc["hotspot_dmr_id"], g_snapshot.hotspot_dmr_id, sizeof(g_snapshot.hotspot_dmr_id));

    g_snapshot.static_tg_count = 0;
    JsonArrayConst tgArray = doc["static_talkgroups"].as<JsonArrayConst>();
    if (!tgArray.isNull())
    {
        for (JsonObjectConst tgItem : tgArray)
        {
            if (g_snapshot.static_tg_count >= (int)MAX_STATIC_TGS)
            {
                break;
            }
            auto &entry = g_snapshot.static_tgs[g_snapshot.static_tg_count++];
            entry.tg = tgItem["tg"] | 0;
            copyJsonString(tgItem["name"], entry.name, sizeof(entry.name));
        }
    }

    g_snapshot.valid = true;

    initializeClockFromSnapshot();
    if (g_currentPage == 0)
    {
        updateHotspotCallsignDisplay();
        updateClockDisplay(true);
        updateFooterStatusDisplay();
    }
    else if (g_currentPage == 2)
    {
        drawStaticTgPage();
    }

    printSnapshot(doc["config"]);
}

void parseLive(JsonDocument &doc)
{
    clearLiveState();

    copyJsonString(doc["type"], g_live.type, sizeof(g_live.type));
    g_live.event_id = doc["event_id"] | 0;
    copyJsonString(doc["timestamp"], g_live.timestamp, sizeof(g_live.timestamp));
    copyJsonString(doc["mode"], g_live.mode, sizeof(g_live.mode));
    copyJsonString(doc["last_event"], g_live.last_event, sizeof(g_live.last_event));
    copyJsonString(doc["direction"], g_live.direction, sizeof(g_live.direction));
    g_live.slot_valid = !doc["slot"].isNull();
    g_live.slot = g_live.slot_valid ? (doc["slot"] | 0) : 0;
    copyJsonString(doc["source"], g_live.source, sizeof(g_live.source));
    g_live.source_match_count = doc["source_match_count"] | 0;
    copyJsonString(doc["source_id"], g_live.source_id, sizeof(g_live.source_id));
    copyJsonString(doc["source_callsign"], g_live.source_callsign, sizeof(g_live.source_callsign));
    copyJsonString(doc["source_name"], g_live.source_name, sizeof(g_live.source_name));
    copyJsonString(doc["source_surname"], g_live.source_surname, sizeof(g_live.source_surname));
    copyJsonString(doc["source_city"], g_live.source_city, sizeof(g_live.source_city));
    copyJsonString(doc["source_state"], g_live.source_state, sizeof(g_live.source_state));
    copyJsonString(doc["source_country"], g_live.source_country, sizeof(g_live.source_country));
    copyJsonString(doc["source_country_code"], g_live.source_country_code, sizeof(g_live.source_country_code));
    copyJsonString(doc["destination"], g_live.destination, sizeof(g_live.destination));
    copyJsonString(doc["talker_alias"], g_live.talker_alias, sizeof(g_live.talker_alias));
    g_live.duration_valid = !doc["duration_sec"].isNull();
    g_live.duration_sec = g_live.duration_valid ? (doc["duration_sec"] | 0.0f) : 0.0f;
    g_live.packet_loss_valid = !doc["packet_loss_percent"].isNull();
    g_live.packet_loss_percent = g_live.packet_loss_valid ? (doc["packet_loss_percent"] | 0.0f) : 0.0f;
    g_live.ber_valid = !doc["ber_percent"].isNull();
    g_live.ber_percent = g_live.ber_valid ? (doc["ber_percent"] | 0.0f) : 0.0f;
    copyJsonString(doc["raw_line"], g_live.raw_line, sizeof(g_live.raw_line));

    JsonArrayConst rssiArray = doc["rssi_values_dbm"].as<JsonArrayConst>();
    g_live.rssi_count = 0;
    if (!rssiArray.isNull())
    {
        for (JsonVariantConst rssiValue : rssiArray)
        {
            if (g_live.rssi_count >= MAX_RSSI_VALUES)
            {
                break;
            }
            g_live.rssi_values_dbm[g_live.rssi_count++] = rssiValue | 0;
        }
    }

    g_live.valid = true;
    if (g_live.slot_valid)
    {
        g_lastFooterSlot = g_live.slot;
        g_lastFooterSlotValid = true;
    }
    if (g_live.rssi_count > 0)
    {
        long rssiTotal = 0;
        for (size_t index = 0; index < g_live.rssi_count; ++index)
        {
            rssiTotal += g_live.rssi_values_dbm[index];
        }
        g_lastKnownRssiDbm = static_cast<int>(rssiTotal / static_cast<long>(g_live.rssi_count));
        g_lastKnownRssiValid = true;
    }
    if (g_live.ber_valid)
    {
        g_lastKnownBerPercent = g_live.ber_percent;
        g_lastKnownBerValid = true;
        g_lastKnownBerUpdatedMs = millis();
    }
    if (g_live.packet_loss_valid)
    {
        g_lastKnownLossPercent = g_live.packet_loss_percent;
        g_lastKnownLossValid = true;
        g_lastKnownLossUpdatedMs = millis();
    }
    if (strcmp(g_live.last_event, "rf_voice_header") == 0 ||
        strcmp(g_live.last_event, "network_voice_header") == 0)
    {
        g_durationTimerActive = true;
        g_durationTimerStartedMs = millis();
        g_lastKnownDurationValid = false;
        g_lastKnownDurationSec = 0.0f;
        g_lastKnownDurationUpdatedMs = 0;
    }
    if (g_live.duration_valid)
    {
        g_durationTimerActive = false;
        g_lastKnownDurationSec = g_live.duration_sec;
        g_lastKnownDurationValid = true;
        g_lastKnownDurationUpdatedMs = millis();
    }
    if (g_currentPage == 0)
    {
        updateActivityStatusBannerDisplay();
        updateRxFlagDisplay();
        updateRxCallsignDisplay();
        updateRxNameDisplay();
        updateRxLocationDisplay();
        updateTalkgroupDisplay();
        updateDurationMetricDisplay();
        updateRssiMetricDisplay();
        updateBerMetricDisplay();
        updateLossMetricDisplay();
        updateFooterStatusDisplay();
    }
    printLive();
}

void parseHeardSummary(JsonDocument &doc)
{
    clearHeardSummaryState();

    copyJsonString(doc["type"], g_heardSummary.type, sizeof(g_heardSummary.type));
    g_heardSummary.unique_callsigns_total = doc["unique_callsigns_total"] | 0;
    copyJsonString(doc["last_heard_callsign"], g_heardSummary.last_heard_callsign, sizeof(g_heardSummary.last_heard_callsign));
    copyJsonString(doc["last_heard_at"], g_heardSummary.last_heard_at, sizeof(g_heardSummary.last_heard_at));

    JsonArrayConst recentArray = doc["recent"].as<JsonArrayConst>();
    g_heardSummary.recent_count = 0;
    if (!recentArray.isNull())
    {
        for (JsonObjectConst recentItem : recentArray)
        {
            if (g_heardSummary.recent_count >= MAX_HEARD_RECENT_ITEMS)
            {
                break;
            }

            HeardRecentItem &item = g_heardSummary.recent[g_heardSummary.recent_count++];
            copyJsonString(recentItem["callsign"], item.callsign, sizeof(item.callsign));
            copyJsonString(recentItem["name"], item.name, sizeof(item.name));
            copyJsonString(recentItem["country_code"], item.country_code, sizeof(item.country_code));
            copyJsonString(recentItem["last_seen"], item.last_seen, sizeof(item.last_seen));
            item.last_seen_unix = recentItem["last_seen_unix"] | (int64_t)0;
            copyJsonString(recentItem["last_tg"], item.last_tg, sizeof(item.last_tg));
        }
    }

    g_heardSummary.valid = true;
    if (g_currentPage == 0)
    {
        updateRecentHeardFlagsDisplay();
        updateFooterStatusDisplay();
    }
    else
    {
        drawHeardListPage();
    }
    printHeardSummary();
}

// Connection helpers

bool primeArp(uint8_t maxAttempts = 3)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WS] ARP prime skipped: WiFi not connected");
        return false;
    }

    WiFiClient tcp;
    for (uint8_t attempt = 1; attempt <= maxAttempts; ++attempt)
    {
        if (tcp.connect(g_wsHost, WS_PORT))
        {
            tcp.stop();
            delay(100);
            return true;
        }
        delay(250);
    }

    Serial.println("[WS] ARP prime skipped: host not reachable yet");
    return false;
}

void configureWebSocketClient()
{
    ws.begin(g_wsHost, WS_PORT, WS_PATH);
    ws.onEvent(onWsEvent);
    ws.setReconnectInterval(5000);
    ws.enableHeartbeat(30000, 10000, 2);
}

void ensureConnectivity()
{
    const unsigned long now = millis();

    if (WiFi.status() != WL_CONNECTED)
    {
        if (now < g_nextWiFiRecoveryMs)
        {
            return;
        }

        g_nextWiFiRecoveryMs = now + WIFI_RECOVERY_INTERVAL_MS;
        Serial.println("[WiFi] Connection lost, attempting reconnect");
        WiFi.disconnect();
        WiFi.reconnect();
    }
    // WebSocket reconnection is handled automatically by the library
    // (setReconnectInterval(5000) in configureWebSocketClient)
}

// WebSocket

void onWsEvent(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        Serial.println("[WS] Disconnected");
        if (g_wsConnected)
        {
            g_wsConnected = false;
            g_wsDisconnectedAtMs = millis();
            g_pendingOfflineDraw = true;
        }
        break;
    case WStype_CONNECTED:
        Serial.println("[WS] Connected");
        g_wsConnected = true;
        g_nextWsRecoveryMs = millis() + WS_RECOVERY_INTERVAL_MS;
        g_pendingReconnectDraw = true;
        {
            // Persist the working IP so next boot can try it first
            Preferences prefs;
            if (prefs.begin("pistar", false))
            {
                prefs.putString("ip", g_wsHost);
                prefs.end();
            }
        }
        break;
    case WStype_TEXT:
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (err)
        {
            Serial.printf("[WS] JSON error: %s\n", err.c_str());
            break;
        }
        const char *t = doc["type"] | "";
        if (strcmp(t, "snapshot") == 0)
            parseSnapshot(doc);
        else if (strcmp(t, "live") == 0)
            parseLive(doc);
        else if (strcmp(t, "heard_summary") == 0)
            parseHeardSummary(doc);
        break;
    }
    default:
        break;
    }
}

#include <SPI.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <XPT2046_Touchscreen.h>

#include "RobotoCondensedBold24px7b.h"
#include "RobotoCondensedBold36px7b.h"
#include "RobotoCondensedBold12px7b.h"
#include "RobotoCondensedRegular10px7b.h"
#include "RobotoMonoRegular10px7b.h"
#include "RobotoMonoRegular12px7b.h"
#include "RobotoMonoRegular20px7b.h"
#include "RobotoCondensedRegular16px7b.h"
#include "UbuntuMonoBold18px7b.h"

TFT_eSPI tft = TFT_eSPI();

SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

#define FONT_SIZE 2

constexpr uint32_t COUNTRY_MAP_DEMO_INTERVAL_MS = 4000;
constexpr int LEFT_COLUMN_SEPARATOR_WIDTH = 222;

const char *kDemoCountryCodes[] = {"CH", "DE", "FR", "GB", "JP", "US"};
constexpr size_t kDemoCountryCodeCount = sizeof(kDemoCountryCodes) / sizeof(kDemoCountryCodes[0]);
const char *kSmallFlagRowDemoCountryCodes[] = {"AU", "BE", "BR", "CA", "CH", "DE", "DK", "ES", "FR", "GB", "IT", "JP", "NL", "SE", "US"};
constexpr size_t kSmallFlagRowDemoCountryCodeCount = sizeof(kSmallFlagRowDemoCountryCodes) / sizeof(kSmallFlagRowDemoCountryCodes[0]);

size_t g_demoCountryIndex = 0;
unsigned long g_nextCountryMapDemoMs = 0;

bool tftJpegOutput(int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t *bitmap)
{
    if (y >= tft.height() || x >= tft.width())
    {
        return false;
    }

    if (x + width > tft.width())
    {
        width = tft.width() - x;
    }

    if (y + height > tft.height())
    {
        height = tft.height() - y;
    }

    tft.pushImage(x, y, width, height, bitmap);
    return true;
}

void displaySplashScreen()
{
    tft.fillScreen(TFT_BLACK);
    TJpgDec.drawFsJpg(0, 0, "/splash_screen.jpg", SPIFFS);
}

bool looksLikeCountryCode(const char *countryCode)
{
    return countryCode != nullptr &&
           strlen(countryCode) == 2 &&
           isAlpha(countryCode[0]) &&
           isAlpha(countryCode[1]);
}

void buildCountryAssetPath(const char *countryCode, const char *sizeFolder, char *path, size_t pathSize)
{
    char normalizedCode[3] = {'\0', '\0', '\0'};
    normalizedCode[0] = static_cast<char>(toupper(countryCode[0]));
    normalizedCode[1] = static_cast<char>(toupper(countryCode[1]));

    snprintf(path, pathSize, "/flags/%s/%s.jpg", sizeFolder, normalizedCode);
}

bool displayCountryMapFromFolder(const char *countryCode, const char *sizeFolder, int x, int y)
{
    if (!looksLikeCountryCode(countryCode))
    {
        Serial.printf("[TFT] Invalid country code: %s\n", countryCode != nullptr ? countryCode : "<null>");
        return false;
    }

    char assetPath[32];
    buildCountryAssetPath(countryCode, sizeFolder, assetPath, sizeof(assetPath));

    if (!SPIFFS.exists(assetPath))
    {
        Serial.printf("[TFT] Missing asset: %s\n", assetPath);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString(countryCode, x + 4, y + 8, FONT_SIZE);
        return false;
    }

    Serial.printf("[TFT] Drawing %s at (%d,%d)\n", assetPath, x, y);
    return TJpgDec.drawFsJpg(x, y, assetPath, SPIFFS);
}

bool displayContryMapLarge(const char *countryCode, int x, int y)
{
    return displayCountryMapFromFolder(countryCode, "large", x, y);
}

bool displayIdleLargeIcon(int x, int y)
{
    static constexpr const char *kIdleAssetPath = "/flags/large/idle.jpg";
    if (!SPIFFS.exists(kIdleAssetPath))
    {
        Serial.printf("[TFT] Missing asset: %s\n", kIdleAssetPath);
        return false;
    }

    return TJpgDec.drawFsJpg(x, y, kIdleAssetPath, SPIFFS);
}

bool displayContryMapSmall(const char *countryCode, int x, int y)
{
    return displayCountryMapFromFolder(countryCode, "small", x, y);
}

bool displayBlankSmallFlag(int x, int y)
{
    static constexpr const char *kBlankSmallJpegPath = "/flags/small/blank.jpeg";
    static constexpr const char *kBlankSmallJpgPath = "/flags/small/blank.jpg";
    static constexpr int kBlankFlagWidth = 24;
    static constexpr int kBlankFlagHeight = 17;

    if (SPIFFS.exists(kBlankSmallJpegPath))
    {
        return TJpgDec.drawFsJpg(x, y, kBlankSmallJpegPath, SPIFFS);
    }

    if (SPIFFS.exists(kBlankSmallJpgPath))
    {
        return TJpgDec.drawFsJpg(x, y, kBlankSmallJpgPath, SPIFFS);
    }

    tft.fillRect(x, y, kBlankFlagWidth, kBlankFlagHeight, TFT_BLACK);
    return false;
}

const char *currentDemoCountryCode()
{
    if (looksLikeCountryCode(g_live.source_country_code))
    {
        return g_live.source_country_code;
    }

    if (looksLikeCountryCode(g_snapshot.station_country_code))
    {
        return g_snapshot.station_country_code;
    }

    return kDemoCountryCodes[g_demoCountryIndex];
}

void drawTopCallsignFontDemo(const char *callsign)
{
    const char *text = (callsign != nullptr && callsign[0] != '\0') ? callsign : "CONNECTING";
    const uint16_t bannerColor = tft.color565(8, 24, 72);
    const int sideMargin = 8;
    const int sideTextY = 7;

    tft.fillRect(0, 0, tft.width(), 32, bannerColor);
    tft.drawFastHLine(0, 31, tft.width(), TFT_CYAN);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, bannerColor);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.drawString("DMR", sideMargin, sideTextY);

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, bannerColor);
    tft.setFreeFont(&UbuntuMonoBold18px7b);
    tft.drawString(text, tft.width() / 2, 5);

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void updateHotspotCallsignDisplay()
{
    const char *callsign = (g_snapshot.valid && g_snapshot.station_callsign[0] != '\0')
                               ? g_snapshot.station_callsign
                               : "CONNECTING";

    drawTopCallsignFontDemo(callsign);
}

bool liveCallsignIsActive()
{
    return strcmp(g_live.last_event, "rf_voice_header") == 0 ||
           strcmp(g_live.last_event, "network_voice_header") == 0 ||
           strcmp(g_live.last_event, "talker_alias") == 0;
}

const char *currentRxCallsignText()
{
    return (g_live.valid && liveCallsignIsActive() && g_live.source_callsign[0] != '\0')
               ? g_live.source_callsign
               : "Idle";
}

const char *currentActivityBannerText()
{
    const char *rxCallsign = currentRxCallsignText();
    if (strcmp(rxCallsign, "Idle") == 0)
    {
        return "IDLE";
    }

    if (g_snapshot.valid &&
        g_snapshot.station_callsign[0] != '\0' &&
        strcmp(rxCallsign, g_snapshot.station_callsign) == 0)
    {
        return "TX";
    }

    return "RX";
}

const char *currentRxNameText()
{
    const char *rxCallsign = currentRxCallsignText();
    if (strcmp(rxCallsign, "Idle") == 0)
    {
        return "";
    }

    if (g_snapshot.valid &&
        g_snapshot.station_callsign[0] != '\0' &&
        strcmp(rxCallsign, g_snapshot.station_callsign) == 0)
    {
        return g_snapshot.station_name;
    }

    return g_live.source_name;
}

void buildRxLocationText(char *destination, size_t destinationSize)
{
    if (destinationSize == 0)
    {
        return;
    }

    destination[0] = '\0';

    const char *rxCallsign = currentRxCallsignText();
    if (strcmp(rxCallsign, "Idle") == 0)
    {
        return;
    }

    const bool isHotspotIdentity = g_snapshot.valid &&
                                   g_snapshot.station_callsign[0] != '\0' &&
                                   strcmp(rxCallsign, g_snapshot.station_callsign) == 0;

    const char *city = isHotspotIdentity ? g_snapshot.station_city : g_live.source_city;
    const char *state = isHotspotIdentity ? g_snapshot.station_state : g_live.source_state;
    const char *country = isHotspotIdentity ? g_snapshot.station_country : g_live.source_country;

    if (city != nullptr && city[0] != '\0')
    {
        strlcpy(destination, city, destinationSize);
    }

    if (country != nullptr && country[0] != '\0')
    {
        if (destination[0] != '\0')
        {
            strlcat(destination, ", ", destinationSize);
        }
        strlcat(destination, country, destinationSize);
    }

    if (destination[0] == '\0' && state != nullptr && state[0] != '\0')
    {
        strlcpy(destination, state, destinationSize);
    }
}

void buildTalkgroupText(char *destination, size_t destinationSize)
{
    if (destinationSize == 0)
    {
        return;
    }

    destination[0] = '\0';

    if (strcmp(currentRxCallsignText(), "Idle") == 0)
    {
        return;
    }

    if (!g_live.slot_valid || g_live.destination[0] == '\0')
    {
        return;
    }

    snprintf(destination, destinationSize, "TS%d %s", g_live.slot, g_live.destination);
}

void updateActivityStatusBannerDisplay()
{
    const char *nextText = currentActivityBannerText();
    if (strcmp(nextText, g_lastActivityBannerText) == 0)
    {
        return;
    }

    drawActivityStatusBannerDemo(nextText);
    strlcpy(g_lastActivityBannerText, nextText, sizeof(g_lastActivityBannerText));
}

void drawRxCallsignText(const char *callsign, uint16_t textColor)
{
    const char *text = (callsign != nullptr && callsign[0] != '\0') ? callsign : "Idle";
    const int textX = 72;
    const int textY = 52;
    const uint16_t idleColor = tft.color565(95, 95, 95);
    const uint16_t color = (textColor == TFT_BLACK)
                               ? TFT_BLACK
                               : (strcmp(text, "Idle") == 0 ? idleColor : textColor);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(color, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold36px7b);
    tft.drawString(text, textX, textY);
    tft.setFreeFont(nullptr);
}

void drawRxCallsignSeparator()
{
    const int separatorY = 103;
    tft.drawFastHLine(0, separatorY, tft.width(), TFT_CYAN);
}

void updateRxCallsignDisplay()
{
    const char *nextText = currentRxCallsignText();

    if (g_lastRxCallsignText[0] != '\0')
    {
        drawRxCallsignText(g_lastRxCallsignText, TFT_BLACK);
    }

    drawRxCallsignText(nextText, TFT_WHITE);
    drawRxCallsignSeparator();
    strlcpy(g_lastRxCallsignText, nextText, sizeof(g_lastRxCallsignText));
}

void updateRxFlagDisplay()
{
    const int flagX = 10;
    const int flagY = 56;

    if (g_live.valid && liveCallsignIsActive() && looksLikeCountryCode(g_live.source_country_code))
    {
        displayContryMapLarge(g_live.source_country_code, flagX, flagY);
        return;
    }

    displayIdleLargeIcon(flagX, flagY);
}

void drawRxNameText(const char *name, uint16_t textColor)
{
    const char *text = (name != nullptr) ? name : "";
    const int x = 10;
    const int y = 109;
    const uint16_t gold = tft.color565(245, 200, 60);
    const uint16_t color = (textColor == TFT_BLACK) ? TFT_BLACK : gold;

    if (text[0] == '\0')
    {
        return;
    }

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(color, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold24px7b);
    tft.drawString(text, x, y);
    tft.setFreeFont(nullptr);
}

void updateRxNameDisplay()
{
    const char *nextText = currentRxNameText();

    if (g_lastRxNameText[0] != '\0')
    {
        drawRxNameText(g_lastRxNameText, TFT_BLACK);
    }

    if (nextText[0] != '\0')
    {
        drawRxNameText(nextText, tft.color565(245, 200, 60));
    }

    drawRxNameSeparator();
    strlcpy(g_lastRxNameText, nextText, sizeof(g_lastRxNameText));
}

void drawRxNameSeparator()
{
    const int dividerY = 144;
    tft.drawFastHLine(0, dividerY, tft.width(), TFT_CYAN);
}

void drawRxLocationText(const char *location, uint16_t textColor)
{
    const char *text = (location != nullptr) ? location : "";
    const int x = 10;
    const int y = 148;
    const uint16_t defaultColor = tft.color565(232, 232, 232);
    const uint16_t color = (textColor == TFT_BLACK) ? TFT_BLACK : defaultColor;

    if (text[0] == '\0')
    {
        return;
    }

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(color, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedRegular16px7b);
    tft.drawString(text, x, y);
    tft.setFreeFont(nullptr);
}

void drawRxLocationSeparator()
{
    const int dividerY = 171;
    tft.drawFastHLine(0, dividerY, tft.width(), TFT_CYAN);
}

void drawTalkgroupSeparators()
{
    const int topSeparatorY = 171;
    const int bottomSeparatorY = 197;
    tft.drawFastHLine(0, topSeparatorY, tft.width(), TFT_CYAN);
    tft.drawFastHLine(0, bottomSeparatorY, tft.width(), TFT_CYAN);
}

void updateRxLocationDisplay()
{
    char nextText[96];
    buildRxLocationText(nextText, sizeof(nextText));

    if (g_lastRxLocationText[0] != '\0')
    {
        drawRxLocationText(g_lastRxLocationText, TFT_BLACK);
    }

    if (nextText[0] != '\0')
    {
        drawRxLocationText(nextText, tft.color565(232, 232, 232));
    }

    drawRxLocationSeparator();
    strlcpy(g_lastRxLocationText, nextText, sizeof(g_lastRxLocationText));
}

void drawTalkgroupText(const char *label, uint16_t textColor)
{
    const char *text = (label != nullptr) ? label : "";
    const int x = 10;
    const int y = 175;
    const uint16_t defaultColor = tft.color565(215, 235, 245);
    const uint16_t color = (textColor == TFT_BLACK) ? TFT_BLACK : defaultColor;

    if (text[0] == '\0')
    {
        return;
    }

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(color, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedRegular16px7b);
    tft.drawString(text, x, y);
    tft.setFreeFont(nullptr);
}

void updateTalkgroupDisplay()
{
    char nextText[48];
    buildTalkgroupText(nextText, sizeof(nextText));

    if (g_lastTalkgroupText[0] != '\0')
    {
        drawTalkgroupText(g_lastTalkgroupText, TFT_BLACK);
    }

    if (nextText[0] != '\0')
    {
        drawTalkgroupText(nextText, tft.color565(215, 235, 245));
    }

    drawTalkgroupSeparators();
    strlcpy(g_lastTalkgroupText, nextText, sizeof(g_lastTalkgroupText));
}

void drawFlagAndCallsignDemo(const char *countryCode, const char *callsign)
{
    const char *code = looksLikeCountryCode(countryCode) ? countryCode : "US";
    const char *text = (callsign != nullptr && callsign[0] != '\0') ? callsign : "Idle";

    const int y = 50;
    const int height = 58;
    const int flagX = 10;
    const int flagY = 56;
    const int textX = 72;
    const int textY = 52;

    if (strcmp(text, "Idle") == 0)
    {
        displayIdleLargeIcon(flagX, flagY);
    }
    else
    {
        displayContryMapLarge(code, flagX, flagY);
    }

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold36px7b);
    tft.drawString(text, textX, textY);
    drawRxCallsignSeparator();

    tft.setFreeFont(nullptr);
}

void drawNameFontDemo(const char *name)
{
    const char *text = (name != nullptr) ? name : "";
    const uint16_t gold = tft.color565(245, 200, 60);

    const int x = 10;
    const int y = 109;
    const int width = tft.width() - x;
    const int height = 34;

    if (text[0] != '\0')
    {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(gold, TFT_BLACK);
        tft.setFreeFont(&RobotoCondensedBold24px7b);
        tft.drawString(text, x, y);
        tft.setFreeFont(nullptr);
    }
}

void drawLocationFontDemo(const char *location)
{
    const char *text = (location != nullptr) ? location : "";
    const uint16_t textColor = tft.color565(232, 232, 232);

    const int x = 10;
    const int y = 150;
    const int width = tft.width() - x;
    const int height = 24;

    drawRxNameSeparator();
    if (text[0] != '\0')
    {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(textColor, TFT_BLACK);
        tft.setFreeFont(&RobotoCondensedRegular16px7b);
        tft.drawString(text, x, y);
        tft.setFreeFont(nullptr);
    }

    drawRxLocationSeparator();
}

void drawTalkgroupRowDemo(const char *label)
{
    const char *text = (label != nullptr) ? label : "";
    const uint16_t textColor = tft.color565(215, 235, 245);

    const int x = 10;
    const int y = 175;
    const int width = tft.width() - x;
    const int height = 28;

    drawTalkgroupSeparators();
    if (text[0] != '\0')
    {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(textColor, TFT_BLACK);
        tft.setFreeFont(&RobotoCondensedRegular16px7b);
        tft.drawString(text, x, y);
        tft.setFreeFont(nullptr);
    }
}

void drawActivityStatusBannerDemo(const char *statusText)
{
    const char *text = (statusText != nullptr && statusText[0] != '\0') ? statusText : "IDLE";
    const bool isIdle = strcmp(text, "IDLE") == 0;
    const bool isTx   = strcmp(text, "TX") == 0;

    const int x          = 10;
    const int y          = 36;
    const int bodyWidth  = 146;
    const int arrowWidth = 12;
    const int height     = 14;
    const int midY       = y + (height / 2);

    const int dotY       = midY;
    const int dotRadius  = 2;
    const int dotCount   = 14;
    const int dotSpacing = 15;
    const int dotStartX  = x + 10;  // left-aligned, shift here to move group

    if (isIdle)
    {
        if (g_idleBlinkPhasePrev < 0)
        {
            // First idle frame: clear arrow area and draw all dots
            tft.fillRect(x - 1, y - 1, bodyWidth + arrowWidth + 2, height + 2, TFT_BLACK);
            for (int i = 0; i < dotCount; i++)
            {
                const uint16_t c = (i == g_idleBlinkPhase)
                    ? tft.color565(210, 210, 210)
                    : tft.color565(50, 50, 50);
                tft.fillCircle(dotStartX + i * dotSpacing, dotY, dotRadius, c);
            }
        }
        else
        {
            // Only update the two changed dots — no fillRect, no flicker
            tft.fillCircle(dotStartX + g_idleBlinkPhasePrev * dotSpacing, dotY, dotRadius, tft.color565(50, 50, 50));
            tft.fillCircle(dotStartX + g_idleBlinkPhase     * dotSpacing, dotY, dotRadius, tft.color565(210, 210, 210));
        }
        g_idleBlinkPhasePrev = g_idleBlinkPhase;
    }
    else
    {
        // Entering RX or TX: reset so next idle entry does full redraw
        g_idleBlinkPhasePrev = -1;

        const uint16_t fillColor = isTx ? tft.color565(210, 45, 45) : tft.color565(35, 145, 60);
        // Clear full dot area (wider than arrow) so no dots bleed through
        const int dotAreaRight = dotStartX + (dotCount - 1) * dotSpacing + dotRadius + 1;
        tft.fillRect(x - 1, y - 1, dotAreaRight - (x - 1) + 1, height + 2, TFT_BLACK);
        tft.fillRect(x, y, bodyWidth, height, fillColor);
        tft.fillTriangle(x + bodyWidth, y,
                         x + bodyWidth + arrowWidth, midY,
                         x + bodyWidth, y + height,
                         fillColor);
        tft.setTextDatum(CC_DATUM);
        tft.setTextColor(TFT_WHITE);
        tft.setFreeFont(&RobotoCondensedRegular10px7b);
        tft.drawString(text, x + (bodyWidth / 2), y + (height / 2) - 1);
    }

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawClockDemo(const char *timeText)
{
    const char *text = (timeText != nullptr && timeText[0] != '\0') ? timeText : "--:--";
    const uint16_t textColor = TFT_WHITE;
    const uint16_t backgroundColor = tft.color565(8, 24, 72);

    const int rightMargin = 8;
    const int y = 7;
    const int width = 86;
    const int height = 14;
    const int x = tft.width() - rightMargin - width;

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(textColor, backgroundColor);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.drawString(text, tft.width() - rightMargin, y);

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawRssiMetricDemo(const char *valueText)
{
    const char *text = (valueText != nullptr && valueText[0] != '\0') ? valueText : "-73";
    const uint16_t labelColor = tft.color565(165, 205, 220);
    const uint16_t valueColor = TFT_WHITE;
    const int x = 246;
    const int x1 = 316;
    const int y = 39;

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(labelColor, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.drawString("RSSI", x, y);
    tft.setTextColor(valueColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(text, x1, y);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawRssiMetricValue(const char *valueText, uint16_t textColor)
{
    const char *text = (valueText != nullptr && valueText[0] != '\0') ? valueText : "--";
    const int x1 = 316;
    const int y = 39;

    tft.setTextColor(textColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(text, x1, y);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void updateRssiMetricDisplay()
{
    char nextText[12] = "--";
    if (g_lastKnownRssiValid)
    {
        snprintf(nextText, sizeof(nextText), "%d", g_lastKnownRssiDbm);
    }

    if (g_lastRssiMetricText[0] != '\0')
    {
        drawRssiMetricValue(g_lastRssiMetricText, TFT_BLACK);
    }

    drawRssiMetricDemo(nextText);
    strlcpy(g_lastRssiMetricText, nextText, sizeof(g_lastRssiMetricText));
}

void drawBerMetricDemo(const char *valueText)
{
    const char *text = (valueText != nullptr && valueText[0] != '\0') ? valueText : "xx";
    const uint16_t labelColor = tft.color565(165, 205, 220);
    const uint16_t valueColor = TFT_WHITE;
    const int x = 246;
    const int x1 = 316;
    const int y = 61;

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(labelColor, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.drawString("BER", x, y);
    tft.setTextColor(valueColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(text, x1, y);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawBerMetricValue(const char *valueText, uint16_t textColor)
{
    const char *text = (valueText != nullptr && valueText[0] != '\0') ? valueText : "--";
    const int x1 = 316;
    const int y = 61;

    tft.setTextColor(textColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(text, x1, y);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void updateBerMetricDisplay()
{
    char nextText[12] = "--";
    const unsigned long now = millis();
    const bool showLastKnownBer = g_lastKnownBerValid &&
                                  (now - g_lastKnownBerUpdatedMs) <= METRIC_HOLD_MS;

    if (showLastKnownBer)
    {
        snprintf(nextText, sizeof(nextText), "%.1f", static_cast<double>(g_lastKnownBerPercent));
    }

    if (strcmp(nextText, g_lastBerMetricText) == 0)
    {
        return;
    }

    if (g_lastBerMetricText[0] != '\0')
    {
        drawBerMetricValue(g_lastBerMetricText, TFT_BLACK);
    }

    drawBerMetricDemo(nextText);
    strlcpy(g_lastBerMetricText, nextText, sizeof(g_lastBerMetricText));
}

void drawLossMetricDemo(const char *valueText)
{
    const char *text = (valueText != nullptr && valueText[0] != '\0') ? valueText : "yy";
    const uint16_t labelColor = tft.color565(165, 205, 220);
    const uint16_t valueColor = TFT_WHITE;
    const int x = 246;
    const int x1 = 316;
    const int y = 83;

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(labelColor, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.drawString("LOSS", x, y);
    tft.setTextColor(valueColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(text, x1, y);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawLossMetricValue(const char *valueText, uint16_t textColor)
{
    const char *text = (valueText != nullptr && valueText[0] != '\0') ? valueText : "--";
    const int x1 = 316;
    const int y = 83;

    tft.setTextColor(textColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular12px7b);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(text, x1, y);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void updateLossMetricDisplay()
{
    char nextText[12] = "--";
    const unsigned long now = millis();
    const bool showLastKnownLoss = g_lastKnownLossValid &&
                                   (now - g_lastKnownLossUpdatedMs) <= METRIC_HOLD_MS;

    if (showLastKnownLoss)
    {
        snprintf(nextText, sizeof(nextText), "%.1f", static_cast<double>(g_lastKnownLossPercent));
    }

    if (strcmp(nextText, g_lastLossMetricText) == 0)
    {
        return;
    }

    if (g_lastLossMetricText[0] != '\0')
    {
        drawLossMetricValue(g_lastLossMetricText, TFT_BLACK);
    }

    drawLossMetricDemo(nextText);
    strlcpy(g_lastLossMetricText, nextText, sizeof(g_lastLossMetricText));
}

void drawDurationMetricDemo(const char *valueText)
{
    const char *text = (valueText != nullptr && valueText[0] != '\0') ? valueText : "0:42";
    const uint16_t valueColor = TFT_WHITE;
    const int x1 = 316;
    const int y = 108;
    const int iconX = x1 - 100;

    if (SPIFFS.exists("/timer.jpg"))
    {
        TJpgDec.drawFsJpg(iconX, y + 1, "/timer.jpg", SPIFFS);
    }
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(valueColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular20px7b);
    tft.drawString(text, x1, y);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawDurationMetricCharacter(char character, int x, int y, uint16_t textColor)
{
    char text[2] = {character, '\0'};

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(textColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular20px7b);
    tft.drawString(text, x, y);
    tft.setFreeFont(nullptr);
}

void formatDurationMetricText(unsigned long totalSeconds, char *destination, size_t destinationSize)
{
    if (destinationSize == 0)
    {
        return;
    }

    const unsigned long minutes = totalSeconds / 60UL;
    const unsigned long seconds = totalSeconds % 60UL;
    snprintf(destination, destinationSize, "%3lu:%02lu", minutes, seconds);
}

int durationMetricCharacterWidth()
{
    static int cachedWidth = 0;
    if (cachedWidth > 0)
    {
        return cachedWidth;
    }

    tft.setFreeFont(&RobotoMonoRegular20px7b);
    cachedWidth = tft.textWidth("000:00") / 6;
    tft.setFreeFont(nullptr);
    return cachedWidth;
}

void updateDurationMetricDisplay()
{
    static constexpr char kDurationPlaceholder[] = "  -:--";
    char nextText[8] = "";
    const unsigned long now = millis();

    if (g_durationTimerActive)
    {
        const unsigned long elapsedMs = now - g_durationTimerStartedMs;
        const unsigned long displaySeconds = elapsedMs == 0 ? 0UL : ((elapsedMs + 999UL) / 1000UL);
        formatDurationMetricText(displaySeconds, nextText, sizeof(nextText));
    }
    else if (g_lastKnownDurationValid &&
             (now - g_lastKnownDurationUpdatedMs) <= METRIC_HOLD_MS)
    {
        const unsigned long wholeSeconds = g_lastKnownDurationSec > 0.0f
                                               ? static_cast<unsigned long>(g_lastKnownDurationSec)
                                               : 0UL;
        formatDurationMetricText(wholeSeconds, nextText, sizeof(nextText));
    }
    else
    {
        strlcpy(nextText, kDurationPlaceholder, sizeof(nextText));
    }

    const uint16_t nextColor = g_durationTimerActive ? tft.color565(90, 220, 120) : TFT_WHITE;
    const bool colorChanged = nextColor != g_lastDurationMetricColor;

    if (!colorChanged && strcmp(nextText, g_lastDurationMetricText) == 0)
    {
        return;
    }

    const int x1 = 316;
    const int y = 108;
    const int iconX = x1 - 100;
    const int charWidth = durationMetricCharacterWidth();
    const size_t charCount = strlen(kDurationPlaceholder);
    const int startX = x1 - (charWidth * static_cast<int>(charCount));

    if (SPIFFS.exists("/timer.jpg"))
    {
        TJpgDec.drawFsJpg(iconX, y + 1, "/timer.jpg", SPIFFS);
    }

    for (size_t index = 0; index < charCount; ++index)
    {
        if (!colorChanged && nextText[index] == g_lastDurationMetricText[index])
        {
            continue;
        }

        const int charX = startX + (static_cast<int>(index) * charWidth);
        if (g_lastDurationMetricText[index] != '\0')
        {
            drawDurationMetricCharacter(g_lastDurationMetricText[index], charX, y, TFT_BLACK);
        }
        drawDurationMetricCharacter(nextText[index], charX, y, nextColor);
    }

    strlcpy(g_lastDurationMetricText, nextText, sizeof(g_lastDurationMetricText));
    g_lastDurationMetricColor = nextColor;
}

void updateRecentHeardFlagsDisplay()
{
    constexpr size_t kVisibleFlagCount = 8;
    constexpr int kFlagWidth = 24;
    const int startX = 17;
    const int stepX = 37;
    const int y = 206;
    char visibleCountryCodes[kVisibleFlagCount][4] = {};
    size_t visibleCount = 0;

    if (g_heardSummary.valid)
    {
        for (size_t recentIndex = 0; recentIndex < g_heardSummary.recent_count && visibleCount < kVisibleFlagCount; ++recentIndex)
        {
            const char *countryCode = g_heardSummary.recent[recentIndex].country_code;
            if (!looksLikeCountryCode(countryCode))
            {
                continue;
            }

            bool alreadyVisible = false;
            for (size_t visibleIndex = 0; visibleIndex < visibleCount; ++visibleIndex)
            {
                if (strcmp(visibleCountryCodes[visibleIndex], countryCode) == 0)
                {
                    alreadyVisible = true;
                    break;
                }
            }

            if (alreadyVisible)
            {
                continue;
            }

            strlcpy(visibleCountryCodes[visibleCount++], countryCode, sizeof(visibleCountryCodes[0]));
        }
    }

    for (size_t index = 0; index < kVisibleFlagCount; ++index)
    {
        const int x = startX + static_cast<int>(index) * stepX;
        if (index < visibleCount)
        {
            displayContryMapSmall(visibleCountryCodes[index], x, y);
        }
        else
        {
            displayBlankSmallFlag(x, y);
        }
    }
}

bool buildFooterFrequencyText(char *destination, size_t destinationSize)
{
    if (destinationSize == 0)
    {
        return false;
    }

    destination[0] = '\0';

    if (!g_snapshot.valid || g_snapshot.config_json_length == 0)
    {
        return false;
    }

    char frequencyValue[24] = "";
    if (!snapshotConfigValue("Info", "RXFrequency", frequencyValue, sizeof(frequencyValue)) || frequencyValue[0] == '\0')
    {
        snapshotConfigValue("Info", "TXFrequency", frequencyValue, sizeof(frequencyValue));
    }

    if (frequencyValue[0] == '\0')
    {
        return false;
    }

    const unsigned long long frequencyHz = strtoull(frequencyValue, nullptr, 10);
    if (frequencyHz == 0)
    {
        return false;
    }

    const unsigned long long mhzWhole = frequencyHz / 1000000ULL;
    const unsigned long long mhzFraction = (frequencyHz % 1000000ULL) / 1000ULL;
    snprintf(destination, destinationSize, "%llu.%03llu MHz",
             mhzWhole,
             mhzFraction);
    return true;
}

void buildFooterStatusText(char *destination, size_t destinationSize)
{
    if (destinationSize == 0)
    {
        return;
    }

    char frequencyText[20] = "--.--- MHz";
    char heardText[24] = "Total heard: 0";

    buildFooterFrequencyText(frequencyText, sizeof(frequencyText));

    if (g_heardSummary.valid)
    {
        snprintf(heardText, sizeof(heardText), "Total heard: %d", g_heardSummary.unique_callsigns_total);
    }

    snprintf(destination, destinationSize, "%s | %s", frequencyText, heardText);
}

void buildWiFiConnectingStatusText(char *destination, size_t destinationSize, unsigned long startMs)
{
    if (destinationSize == 0)
    {
        return;
    }

    static constexpr char kDotFrames[][4] = {"", ".", "..", "..."};
    const unsigned long elapsedSeconds = (millis() - startMs) / 1000UL;
    const size_t dotFrameIndex = (millis() / 400UL) % 4U;

    snprintf(destination,
             destinationSize,
             "WiFi connecting%s %lus",
             kDotFrames[dotFrameIndex],
             elapsedSeconds);
}

void updateFooterStatusTextDisplay(const char *statusText, uint16_t textColor)
{
    const char *nextText = (statusText != nullptr && statusText[0] != '\0') ? statusText : "--.--- MHz | Total heard: 0";

    if (strcmp(nextText, g_lastFooterStatusText) == 0)
    {
        return;
    }

    // Clear the full footer band before redrawing to avoid pixel artifacts
    tft.fillRect(0, 227, tft.width(), 14, TFT_BLACK);

    drawFooterStatusText(nextText, textColor);
    strlcpy(g_lastFooterStatusText, nextText, sizeof(g_lastFooterStatusText));
}

void updateFooterStatusDisplay()
{
    char nextText[64];
    buildFooterStatusText(nextText, sizeof(nextText));
    updateFooterStatusTextDisplay(nextText, tft.color565(205, 215, 225));
}

void drawFooterStatusDemo(const char *statusText)
{
    drawFooterStatusText(statusText, tft.color565(205, 215, 225));
}

void drawFooterStatusText(const char *statusText, uint16_t textColor)
{
    const char *text = (statusText != nullptr && statusText[0] != '\0') ? statusText : "--.--- MHz | TS- | 0 heard";

    const int y = 228;
    const int height = 12;

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(textColor, TFT_BLACK);
    tft.setFreeFont(&RobotoMonoRegular10px7b);
    tft.drawString(text, tft.width() / 2, y);

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void renderCountryMapDemo(const char *countryCode)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Country map test", 8, 8, FONT_SIZE);
    tft.drawString(countryCode, 230, 8, FONT_SIZE);

    displayContryMapLarge(countryCode, 8, 40);
    displayContryMapSmall(countryCode, 220, 40);
}

void updateCountryMapDemo()
{
    const unsigned long now = millis();
    if (now < g_nextCountryMapDemoMs)
    {
        return;
    }

    const bool usingLiveCode = looksLikeCountryCode(g_live.source_country_code) ||
                               looksLikeCountryCode(g_snapshot.station_country_code);
    const char *countryCode = currentDemoCountryCode();
    renderCountryMapDemo(countryCode);

    if (!usingLiveCode)
    {
        g_demoCountryIndex = (g_demoCountryIndex + 1) % kDemoCountryCodeCount;
    }

    g_nextCountryMapDemoMs = now + COUNTRY_MAP_DEMO_INTERVAL_MS;
}

// Print Touchscreen info about X, Y and Pressure (Z) on the Serial Monitor
void printTouchToSerial(int touchX, int touchY, int touchZ)
{
    Serial.print("X = ");
    Serial.print(touchX);
    Serial.print(" | Y = ");
    Serial.print(touchY);
    Serial.print(" | Pressure = ");
    Serial.print(touchZ);
    Serial.println();
}

// Print Touchscreen info about X, Y and Pressure (Z) on the TFT Display
void printTouchToDisplay(int touchX, int touchY, int touchZ)
{
    // Clear TFT screen
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);

    int centerX = tft.width() / 2;
    int textY = 80;

    String tempText = "X = " + String(touchX);
    tft.drawCentreString(tempText, centerX, textY, FONT_SIZE);

    textY += 20;
    tempText = "Y = " + String(touchY);
    tft.drawCentreString(tempText, centerX, textY, FONT_SIZE);

    textY += 20;
    tempText = "Pressure = " + String(touchZ);
    tft.drawCentreString(tempText, centerX, textY, FONT_SIZE);
}

// ---- Page 2: Last Heard List ----

void resetDisplayCache()
{
    g_lastClockText[0] = '\0';
    g_lastRxCallsignText[0] = '\0';
    g_lastRxNameText[0] = '\0';
    g_lastRxLocationText[0] = '\0';
    g_lastTalkgroupText[0] = '\0';
    g_lastRssiMetricText[0] = '\0';
    g_lastBerMetricText[0] = '\0';
    g_lastLossMetricText[0] = '\0';
    g_lastDurationMetricText[0] = '\0';
    g_lastDurationMetricColor = 0;
    g_lastActivityBannerText[0] = '\0';
    g_lastFooterStatusText[0] = '\0';
}

void drawHeardListPageHeader()
{
    const uint16_t bannerColor = tft.color565(8, 24, 72);
    tft.fillRect(0, 0, tft.width(), 32, bannerColor);
    tft.drawFastHLine(0, 31, tft.width(), TFT_CYAN);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, bannerColor);
    tft.setFreeFont(&UbuntuMonoBold18px7b);
    tft.drawString("LAST HEARD", 8, 5);

    if (g_heardSummary.valid && g_heardSummary.unique_callsigns_total > 0)
    {
        char countText[24];
        snprintf(countText, sizeof(countText), "%d total", g_heardSummary.unique_callsigns_total);
        tft.setTextDatum(TR_DATUM);
        tft.setFreeFont(&RobotoMonoRegular10px7b);
        tft.drawString(countText, tft.width() - 8, 10);
    }

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawHeardListRow(size_t rowIndex, const HeardRecentItem &item, int rowY)
{
    constexpr int kRowHeight = 20;
    constexpr int kFlagX = 4;
    constexpr int kCallsignX = 34;
    constexpr int kNameX = 110;
    constexpr int kTgX = 205;
    constexpr int kTimeX1 = 315;

    tft.fillRect(0, rowY, tft.width(), kRowHeight, TFT_BLACK);
    tft.drawFastHLine(0, rowY + kRowHeight - 1, tft.width(), tft.color565(28, 28, 44));

    if (looksLikeCountryCode(item.country_code))
    {
        displayContryMapSmall(item.country_code, kFlagX, rowY + 1);
    }
    else
    {
        displayBlankSmallFlag(kFlagX, rowY + 1);
    }

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.drawString(item.callsign, kCallsignX, rowY + 2);

    tft.setFreeFont(&RobotoCondensedRegular10px7b);
    tft.setTextColor(tft.color565(190, 190, 190), TFT_BLACK);
    tft.drawString(item.name, kNameX, rowY + 3);

    if (item.last_tg[0] != '\0')
    {
        tft.setTextColor(tft.color565(160, 140, 80), TFT_BLACK);
        tft.drawString(item.last_tg, kTgX, rowY + 3);
    }

    char timeText[16];
    buildLastSeenTimeText(item.last_seen_unix, timeText, sizeof(timeText));
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(tft.color565(80, 200, 240));
    tft.setFreeFont(&RobotoMonoRegular10px7b);
    tft.drawString(timeText, kTimeX1, rowY + 3);

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawHeardListPage()
{
    tft.fillScreen(TFT_BLACK);
    drawHeardListPageHeader();

    if (!g_heardSummary.valid || g_heardSummary.recent_count == 0)
    {
        tft.setTextDatum(CC_DATUM);
        tft.setTextColor(tft.color565(120, 120, 120), TFT_BLACK);
        tft.setFreeFont(&RobotoCondensedRegular16px7b);
        tft.drawString("No stations heard yet", tft.width() / 2, tft.height() / 2);
        tft.setFreeFont(nullptr);
        tft.setTextDatum(TL_DATUM);
        g_page2LastRefreshMs = millis();
        return;
    }

    constexpr int kRowHeight = 20;
    const int startY = 32;

    for (size_t i = 0; i < g_heardSummary.recent_count; ++i)
    {
        drawHeardListRow(i, g_heardSummary.recent[i], startY + static_cast<int>(i) * kRowHeight);
    }

    g_page2LastRefreshMs = millis();
}

void switchToPage(int page)
{
    if (page == g_currentPage)
    {
        return;
    }

    g_currentPage = page;

    if (g_currentPage == 0)
    {
        g_idleBlinkPhase = 0;
        g_idleBlinkPhasePrev = -1;
        g_idleBlinkMs = millis();
        tft.fillScreen(TFT_BLACK);
        resetDisplayCache();
        updateHotspotCallsignDisplay();
        updateClockDisplay(true);
        updateActivityStatusBannerDisplay();
        updateRxFlagDisplay();
        updateRxCallsignDisplay();
        updateRxNameDisplay();
        updateRxLocationDisplay();
        updateTalkgroupDisplay();
        updateDurationMetricDisplay();
        updateRssiMetricDisplay();
        updateBerMetricDisplay();
        updateLossMetricDisplay();
        updateRecentHeardFlagsDisplay();
        updateFooterStatusDisplay();
    }
    else if (g_currentPage == 1)
    {
        drawHeardListPage();
    }
    else if (g_currentPage == 2)
    {
        g_staticTgScrollOffset = 0;
        drawStaticTgPage();
    }
    else if (g_currentPage == 3)
    {
        drawHotspotInfoPage();
    }
}

// ---- Page 3: Static Talkgroups ----

void drawStaticTgPageHeader()
{
    const uint16_t bannerColor = tft.color565(8, 24, 72);
    tft.fillRect(0, 0, tft.width(), 32, bannerColor);
    tft.drawFastHLine(0, 31, tft.width(), TFT_CYAN);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, bannerColor);
    tft.setFreeFont(&UbuntuMonoBold18px7b);
    tft.drawString("STATIC TGs", 8, 5);

    if (g_snapshot.hotspot_dmr_id[0] != '\0')
    {
        tft.setTextDatum(TR_DATUM);
        tft.setFreeFont(&RobotoMonoRegular10px7b);
        tft.drawString(g_snapshot.hotspot_dmr_id, tft.width() - 8, 10);
    }

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawStaticTgRow(int rowIndex, int tg, const char *name, int rowY)
{
    constexpr int kRowHeight = 26;
    tft.fillRect(0, rowY, tft.width(), kRowHeight, TFT_BLACK);
    tft.drawFastHLine(0, rowY + kRowHeight - 1, tft.width(), tft.color565(28, 28, 44));

    char tgLabel[12];
    snprintf(tgLabel, sizeof(tgLabel), "TG %d", tg);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tft.color565(80, 200, 240), TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.drawString(tgLabel, 10, rowY + 7);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.drawString(name, 120, rowY + 7);

    tft.setFreeFont(nullptr);
}

void drawStaticTgPage()
{
    tft.fillScreen(TFT_BLACK);
    drawStaticTgPageHeader();

    if (!g_snapshot.valid || g_snapshot.static_tg_count == 0)
    {
        tft.setTextDatum(CC_DATUM);
        tft.setTextColor(tft.color565(120, 120, 120), TFT_BLACK);
        tft.setFreeFont(&RobotoCondensedRegular16px7b);
        tft.drawString("No static TGs configured", tft.width() / 2, tft.height() / 2);
        tft.setFreeFont(nullptr);
        tft.setTextDatum(TL_DATUM);
        return;
    }

    constexpr int kRowHeight = 26;
    constexpr int kScrollBarH = 26;
    constexpr int kStartY = 32;
    const int total = g_snapshot.static_tg_count;
    const bool needsScroll = total > 8;
    const int maxVisible = needsScroll ? 7 : 8;

    // Clamp scroll offset
    const int maxOffset = needsScroll ? (total - maxVisible) : 0;
    if (g_staticTgScrollOffset < 0) g_staticTgScrollOffset = 0;
    if (g_staticTgScrollOffset > maxOffset) g_staticTgScrollOffset = maxOffset;

    for (int i = 0; i < maxVisible && (g_staticTgScrollOffset + i) < total; ++i)
    {
        int idx = g_staticTgScrollOffset + i;
        drawStaticTgRow(idx, g_snapshot.static_tgs[idx].tg, g_snapshot.static_tgs[idx].name,
                        kStartY + i * kRowHeight);
    }

    if (needsScroll)
    {
        const int barY = tft.height() - kScrollBarH;
        const uint16_t barColor = tft.color565(20, 20, 40);
        tft.fillRect(0, barY, tft.width(), kScrollBarH, barColor);
        tft.drawFastHLine(0, barY, tft.width(), tft.color565(60, 60, 100));

        // Up arrow (left)
        const bool canUp = g_staticTgScrollOffset > 0;
        tft.setTextDatum(CL_DATUM);
        tft.setTextColor(canUp ? TFT_WHITE : tft.color565(60, 60, 80), barColor);
        tft.setFreeFont(&RobotoCondensedBold12px7b);
        tft.drawString("< UP", 10, barY + kScrollBarH / 2);

        // Position indicator (center)
        char pos[16];
        snprintf(pos, sizeof(pos), "%d-%d / %d",
                 g_staticTgScrollOffset + 1,
                 min(g_staticTgScrollOffset + maxVisible, total),
                 total);
        tft.setTextDatum(CC_DATUM);
        tft.setTextColor(tft.color565(160, 160, 200), barColor);
        tft.drawString(pos, tft.width() / 2, barY + kScrollBarH / 2);

        // Down arrow (right)
        const bool canDown = g_staticTgScrollOffset < maxOffset;
        tft.setTextDatum(CR_DATUM);
        tft.setTextColor(canDown ? TFT_WHITE : tft.color565(60, 60, 80), barColor);
        tft.drawString("DN >", tft.width() - 10, barY + kScrollBarH / 2);

        tft.setFreeFont(nullptr);
        tft.setTextDatum(TL_DATUM);
    }
}

// ---- Page 4: Hotspot Info ----

static void drawInfoRow(int y, const char *label, const char *value, uint16_t valueColor)
{
    constexpr int kRowH   = 22;
    constexpr int kLabelX = 8;
    constexpr int kValueX = 108;

    tft.fillRect(0, y, tft.width(), kRowH, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&RobotoCondensedRegular10px7b);
    tft.setTextColor(tft.color565(110, 110, 110), TFT_BLACK);
    tft.drawString(label, kLabelX, y + 6);
    tft.setTextColor(valueColor, TFT_BLACK);
    tft.drawString(value, kValueX, y + 6);
}

void drawHotspotInfoPage()
{
    constexpr int kRowH    = 22;
    constexpr int kStartY  = 36;
    constexpr int kLabelX  = 8;
    constexpr int kValueX  = 108;
    constexpr uint16_t kDim   = 0x39E7; // color565(110,110,110)

    tft.fillScreen(TFT_BLACK);

    // Header
    const uint16_t bannerColor = tft.color565(8, 24, 72);
    tft.fillRect(0, 0, tft.width(), 32, bannerColor);
    tft.drawFastHLine(0, 31, tft.width(), TFT_CYAN);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, bannerColor);
    tft.setFreeFont(&UbuntuMonoBold18px7b);
    tft.drawString("HOTSPOT INFO", 8, 5);

    if (!g_snapshot.valid)
    {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.setFreeFont(&RobotoCondensedRegular10px7b);
        tft.drawString("No data", tft.width() / 2, tft.height() / 2);
        tft.setTextDatum(TL_DATUM);
        tft.setFreeFont(nullptr);
        return;
    }

    int y = kStartY;

    // Row 1: flag + callsign (left) | hotspot DMR ID (right)
    if (looksLikeCountryCode(g_snapshot.station_country_code))
        displayContryMapSmall(g_snapshot.station_country_code, kLabelX, y + 3);
    else
        displayBlankSmallFlag(kLabelX, y + 3);

    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(g_snapshot.station_callsign[0] ? g_snapshot.station_callsign : "—", 34, y + 2);

    if (g_snapshot.hotspot_dmr_id[0])
    {
        tft.setTextDatum(TR_DATUM);
        tft.setFreeFont(&RobotoMonoRegular10px7b);
        tft.setTextColor(tft.color565(80, 200, 240), TFT_BLACK);
        tft.drawString(g_snapshot.hotspot_dmr_id, 314, y + 5);
        tft.setTextDatum(TL_DATUM);
    }
    y += kRowH;

    // Row 2: Operator name
    {
        char fullName[72] = "—";
        if (g_snapshot.station_name[0])
            snprintf(fullName, sizeof(fullName), "%s %s", g_snapshot.station_name, g_snapshot.station_surname);
        drawInfoRow(y, "Operator", fullName, tft.color565(220, 220, 220));
        y += kRowH;
    }

    // Row 3: QTH
    {
        char qth[72] = "—";
        if (g_snapshot.station_city[0])
            snprintf(qth, sizeof(qth), "%s, %s", g_snapshot.station_city, g_snapshot.station_country);
        drawInfoRow(y, "QTH", qth, tft.color565(200, 200, 200));
        y += kRowH;
    }

    // Separator
    tft.drawFastHLine(0, y + 2, tft.width(), tft.color565(30, 30, 50));
    y += 8;

    // Row 4: RX frequency
    {
        char rxFreq[24] = "—";
        char raw[24]    = "";
        if (snapshotConfigValue("Info", "RXFrequency", raw, sizeof(raw)) && raw[0])
        {
            unsigned long long hz = strtoull(raw, nullptr, 10);
            if (hz)
                snprintf(rxFreq, sizeof(rxFreq), "%llu.%03llu MHz",
                         hz / 1000000ULL, (hz % 1000000ULL) / 1000ULL);
        }
        drawInfoRow(y, "RX Freq", rxFreq, tft.color565(80, 240, 120));
        y += kRowH;
    }

    // Row 5: TX frequency
    {
        char txFreq[24] = "—";
        char raw[24]    = "";
        if (snapshotConfigValue("Info", "TXFrequency", raw, sizeof(raw)) && raw[0])
        {
            unsigned long long hz = strtoull(raw, nullptr, 10);
            if (hz)
                snprintf(txFreq, sizeof(txFreq), "%llu.%03llu MHz",
                         hz / 1000000ULL, (hz % 1000000ULL) / 1000ULL);
        }
        drawInfoRow(y, "TX Freq", txFreq, tft.color565(80, 240, 120));
        y += kRowH;
    }

    // Row 6: Color Code
    {
        char cc[8] = "—";
        snapshotConfigValue("DMR", "ColorCode", cc, sizeof(cc));
        drawInfoRow(y, "Color Code", cc, TFT_WHITE);
        y += kRowH;
    }

    // Row 7: Service state
    {
        const char *state = g_snapshot.service_state[0] ? g_snapshot.service_state : "—";
        const bool active = (strcmp(state, "active") == 0);
        const uint16_t stateColor = active ? tft.color565(80, 240, 120) : tft.color565(240, 80, 80);
        drawInfoRow(y, "Service", state, stateColor);
    }

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

// ---- Offline overlay ----

void updateOfflineElapsed()
{
    const unsigned long elapsedSec = (millis() - g_wsDisconnectedAtMs) / 1000UL;
    const unsigned long mins = elapsedSec / 60;
    const unsigned long secs = elapsedSec % 60;

    char buf[24];
    if (mins > 0)
        snprintf(buf, sizeof(buf), "offline %lum %02lus", mins, secs);
    else
        snprintf(buf, sizeof(buf), "offline %lus", secs);

    tft.setTextDatum(MC_DATUM);
    tft.setFreeFont(&RobotoMonoRegular10px7b);
    tft.setTextColor(tft.color565(70, 70, 70), TFT_BLACK);
    tft.fillRect(0, 210, tft.width(), 16, TFT_BLACK);
    tft.drawString(buf, tft.width() / 2, 218);
    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

void drawOfflinePage()
{
    tft.fillScreen(TFT_BLACK);

    tft.setTextDatum(MC_DATUM);

    tft.setFreeFont(&RobotoCondensedBold24px7b);
    tft.setTextColor(tft.color565(170, 170, 170), TFT_BLACK);
    tft.drawString("HOTSPOT", tft.width() / 2, 75);

    tft.setFreeFont(&RobotoCondensedBold36px7b);
    tft.setTextColor(tft.color565(220, 60, 30), TFT_BLACK);
    tft.drawString("OFFLINE", tft.width() / 2, 120);

    tft.setFreeFont(&RobotoCondensedRegular10px7b);
    tft.setTextColor(tft.color565(100, 100, 100), TFT_BLACK);
    tft.drawString("Reconnecting...", tft.width() / 2, 186);

    updateOfflineElapsed();

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

// ---- Pi-Star Discovery ----

// Step indices
constexpr int kDiscStepNvs   = 0;
constexpr int kDiscStepMdns  = 1;
constexpr int kDiscStepScan  = 2;
constexpr int kDiscStepFallback = 3;

static const uint16_t kDiscColorTrying  = 0xFFFF; // white
static const uint16_t kDiscColorOk      = 0x07E0; // green
static const uint16_t kDiscColorFail    = 0xF800; // red
static const uint16_t kDiscColorSkipped = 0x7BEF; // grey

static int  kDiscRowY[4];   // y positions set in drawDiscoveryScreen()
static bool kDiscScreenUp = false;

static void drawDiscoveryScreen()
{
    tft.fillScreen(TFT_BLACK);

    // Title
    tft.setFreeFont(&UbuntuMonoBold18px7b);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("PI-STAR DISCOVERY", tft.width() / 2, 8);
    tft.drawFastHLine(0, 32, tft.width(), tft.color565(0, 80, 80));

    // Row positions
    kDiscRowY[kDiscStepNvs]      = 48;
    kDiscRowY[kDiscStepMdns]     = 95;
    kDiscRowY[kDiscStepScan]     = 142;
    kDiscRowY[kDiscStepFallback] = 195;

    // Step labels
    const char *labels[4] = {
        "Last known IP",
        "mDNS pi-star.local",
        "Network scan",
        "Not found — retry"
    };
    for (int i = 0; i < 4; i++)
    {
        tft.setFreeFont(&RobotoCondensedBold12px7b);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(tft.color565(160, 160, 160), TFT_BLACK);
        tft.drawString(labels[i], 8, kDiscRowY[i]);
    }

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
    kDiscScreenUp = true;
}

// Update a step's status badge and optional detail line below it
static void updateDiscoveryStep(int step, const char *status, uint16_t color, const char *detail = nullptr)
{
    const int y = kDiscRowY[step];

    // Clear badge area (right side)
    tft.fillRect(200, y - 2, tft.width() - 200, 16, TFT_BLACK);
    tft.setFreeFont(&RobotoCondensedBold12px7b);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(status, tft.width() - 6, y);

    // Detail line (smaller, below label)
    tft.fillRect(8, y + 16, tft.width() - 8, 16, TFT_BLACK);
    if (detail && detail[0])
    {
        tft.setFreeFont(&RobotoCondensedRegular10px7b);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(detail, 16, y + 16);
    }

    tft.setFreeFont(nullptr);
    tft.setTextDatum(TL_DATUM);
}

// Try connecting to port 8765 on a given IP with a short timeout
static bool probeHost(const char *ip, uint16_t timeoutMs = 300)
{
    WiFiClient tcp;
    bool ok = tcp.connect(ip, WS_PORT, timeoutMs);
    tcp.stop();
    return ok;
}

bool discoverPiStar()
{
    drawDiscoveryScreen();

    // ── Step 1: Last known IP from NVS ──────────────────────────────
    if (!kSkipFastDiscovery)
    {
        updateDiscoveryStep(kDiscStepNvs, "trying...", kDiscColorTrying);
        Preferences prefs;
        String savedIp = "";
        if (prefs.begin("pistar", true))
        {
            savedIp = prefs.getString("ip", "");
            prefs.end();
        }
        if (savedIp.length() >= 7)
        {
            char ipBuf[40];
            strlcpy(ipBuf, savedIp.c_str(), sizeof(ipBuf));
            if (probeHost(ipBuf, 500))
            {
                strlcpy(g_wsHost, ipBuf, sizeof(g_wsHost));
                updateDiscoveryStep(kDiscStepNvs, "FOUND", kDiscColorOk, g_wsHost);
                updateDiscoveryStep(kDiscStepMdns,     "skipped", kDiscColorSkipped);
                updateDiscoveryStep(kDiscStepScan,     "skipped", kDiscColorSkipped);
                updateDiscoveryStep(kDiscStepFallback, "skipped", kDiscColorSkipped);
                delay(3000);
                return true;
            }
            updateDiscoveryStep(kDiscStepNvs, "no reply", kDiscColorFail, ipBuf);
        }
        else
        {
            updateDiscoveryStep(kDiscStepNvs, "not saved", kDiscColorSkipped);
        }
    }
    else
    {
        updateDiscoveryStep(kDiscStepNvs, "skipped", kDiscColorSkipped, "(test mode)");
    }

    // ── Step 2: mDNS pi-star.local ──────────────────────────────────
    if (!kSkipFastDiscovery)
    {
        updateDiscoveryStep(kDiscStepMdns, "trying...", kDiscColorTrying);
        MDNS.begin("esp32-pistar");
        IPAddress mdnsIP = MDNS.queryHost("pi-star", 3000);
        if (mdnsIP != INADDR_NONE && (uint32_t)mdnsIP != 0)
        {
            strlcpy(g_wsHost, mdnsIP.toString().c_str(), sizeof(g_wsHost));
            updateDiscoveryStep(kDiscStepMdns, "FOUND", kDiscColorOk, g_wsHost);
            updateDiscoveryStep(kDiscStepScan,     "skipped", kDiscColorSkipped);
            updateDiscoveryStep(kDiscStepFallback, "skipped", kDiscColorSkipped);
            delay(1200);
            return true;
        }
        updateDiscoveryStep(kDiscStepMdns, "not found", kDiscColorFail);
    }
    else
    {
        updateDiscoveryStep(kDiscStepMdns, "skipped", kDiscColorSkipped, "(test mode)");
    }

    // ── Step 3: Network scan ─────────────────────────────────────────
    // Wait until gateway is reachable before scanning — routing/ARP is not
    // ready immediately after WiFi connects (all hosts return EHOSTUNREACH)
    {
        updateDiscoveryStep(kDiscStepScan, "waiting...", kDiscColorTrying, "checking gateway");
        IPAddress gw = WiFi.gatewayIP();
        char gwStr[20];
        snprintf(gwStr, sizeof(gwStr), "%d.%d.%d.%d", gw[0], gw[1], gw[2], gw[3]);
        bool gwReady = false;
        for (int attempt = 0; attempt < 20 && !gwReady; attempt++)
        {
            WiFiClient gwClient;
            // port 80 likely closed on router, but "connection refused" means routing works
            gwClient.connect(gw, 80, 500);
            gwClient.stop();
            // If WiFi is still connected after the probe, routing is up
            if (WiFi.status() == WL_CONNECTED)
                gwReady = true;
            else
                delay(500);
        }
    }
    // Wait until DHCP has assigned a valid IP (localIP can be 0.0.0.0
    // briefly after WiFi connects even when status == WL_CONNECTED)
    {
        IPAddress ip;
        for (int w = 0; w < 20; w++)
        {
            ip = WiFi.localIP();
            if ((uint32_t)ip != 0) break;
            delay(500);
        }
        if ((uint32_t)WiFi.localIP() == 0)
        {
            updateDiscoveryStep(kDiscStepScan, "no IP", kDiscColorFail);
            delay(2000);
            return false;
        }
    }
    updateDiscoveryStep(kDiscStepScan, "scanning...", kDiscColorTrying);

    IPAddress localIP  = WiFi.localIP();
    IPAddress subnet   = WiFi.subnetMask();
    IPAddress network  = IPAddress(localIP[0] & subnet[0],
                                   localIP[1] & subnet[1],
                                   localIP[2] & subnet[2],
                                   localIP[3] & subnet[3]);

    char scanDetail[40];
    bool found = false;
    int total = 0, checked = 0;

    // Count scannable hosts
    for (int i = 1; i <= 254; i++)
    {
        IPAddress candidate(network[0], network[1], network[2], i);
        if (candidate != localIP && candidate != WiFi.gatewayIP())
            total++;
    }

    for (int i = 1; i <= 254 && !found; i++)
    {
        IPAddress candidate(network[0], network[1], network[2], i);
        if (candidate == localIP || candidate == WiFi.gatewayIP())
            continue;

        checked++;
        snprintf(scanDetail, sizeof(scanDetail), "%d.%d.%d.%d  (%d/%d)",
                 candidate[0], candidate[1], candidate[2], candidate[3],
                 checked, total);
        updateDiscoveryStep(kDiscStepScan, "scanning...", kDiscColorTrying, scanDetail);

        char ipStr[20];
        snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d",
                 candidate[0], candidate[1], candidate[2], candidate[3]);

        if (probeHost(ipStr, 300))
        {
            strlcpy(g_wsHost, ipStr, sizeof(g_wsHost));
            updateDiscoveryStep(kDiscStepScan, "FOUND", kDiscColorOk, g_wsHost);
            updateDiscoveryStep(kDiscStepFallback, "skipped", kDiscColorSkipped);
            delay(1200);
            found = true;
        }
        else
        {
            delay(10); // let socket release before next probe
        }
    }

    if (found)
        return true;

    updateDiscoveryStep(kDiscStepScan, "not found", kDiscColorFail);

    // ── Step 4: Not found — retry after countdown ────────────────────
    updateDiscoveryStep(kDiscStepFallback, "retrying...", kDiscColorFail);
    for (int s = 15; s > 0; s--)
    {
        char msg[24];
        snprintf(msg, sizeof(msg), "retrying in %ds...", s);
        updateDiscoveryStep(kDiscStepFallback, msg, kDiscColorFail);
        delay(1000);
    }
    return false; // caller loops back into discoverPiStar()
}

// Setup / loop

void setup()
{
    Serial.begin(115200);

    // Start the SPI for the touchscreen and init the touchscreen
    touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchscreenSPI);
    // Set the Touchscreen rotation in landscape mode
    // Note: in some displays, the touchscreen might be upside down, so you might need to set the rotation to 3: touchscreen.setRotation(3);
    touchscreen.setRotation(1);

    // Start the tft display
    tft.init();
    // Set the TFT display rotation in landscape mode
    tft.setRotation(1);
    tft.setSwapBytes(true);

    if (!SPIFFS.begin(true))
    {
        Serial.println("[SPIFFS] Mount failed");
    }
    else
    {
        Serial.println("[SPIFFS] Mounted");
    }

    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(tftJpegOutput);

    randomSeed(micros());
    displaySplashScreen();

    clearSnapshotState();
    clearLiveState();
    clearHeardSummaryState();

    const uint16_t wifiStatusColor = tft.color565(240, 210, 120);
    char wifiStatusText[64];

    snprintf(wifiStatusText, sizeof(wifiStatusText), "WiFi starting...");
    updateFooterStatusTextDisplay(wifiStatusText, wifiStatusColor);

    // Factory reset: hold touch at boot for 3 s to clear all saved credentials
    {
        TS_Point p = touchscreen.getPoint();
        if (p.z > 200)
        {
            unsigned long holdStart = millis();
            bool held = true;
            while (millis() - holdStart < 3000)
            {
                TS_Point p2 = touchscreen.getPoint();
                if (p2.z < 200) { held = false; break; }
                delay(10);
            }
            if (held)
            {
                Serial.println("[Boot] Factory reset triggered by touch — erasing NVS.");
                HB9IIUPortal::eraseAllPreferencesAndRestart();
            }
        }
    }

    // WiFi connect loop:
    //   - No credentials saved  → start captive portal (AP mode), wait until configured
    //   - Credentials exist     → keep retrying; touch screen 3s on error screen to factory reset
    //   - Connected             → proceed
    while (true)
    {
        HB9IIUPortal::begin("esp32-pistar");

        if (HB9IIUPortal::isConnected())
            break; // success

        if (HB9IIUPortal::isInAPMode())
        {
            // First boot / factory reset: no credentials → serve portal until user configures
            while (!HB9IIUPortal::isConnected())
                HB9IIUPortal::loop();
            break;
        }

        // Credentials exist but connection failed — check for factory-reset touch, then retry
        {
            TS_Point p = touchscreen.getPoint();
            if (p.z > 200)
            {
                unsigned long holdStart = millis();
                bool held = true;
                while (millis() - holdStart < 3000)
                {
                    if (touchscreen.getPoint().z < 200) { held = false; break; }
                    delay(10);
                }
                if (held) HB9IIUPortal::eraseAllPreferencesAndRestart();
            }
        }
        // loop → retry
    }
    WiFi.setAutoReconnect(true);

    snprintf(wifiStatusText, sizeof(wifiStatusText),
             "WiFi connected %s", WiFi.localIP().toString().c_str());
    updateFooterStatusTextDisplay(wifiStatusText, wifiStatusColor);
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());

    while (!discoverPiStar()); // retries until Pi-Star is found
    Serial.printf("[Discovery] Using Pi-Star host: %s\n", g_wsHost);

    primeArp();

    configureWebSocketClient();

    g_wsDisconnectedAtMs = millis();
    drawOfflinePage();
}
// Touchscreen coordinates: (x, y) and pressure (z)
int x, y, z;
void loop()
{
    ensureConnectivity();
    ws.loop();

    // Handle deferred display updates (must not be called from inside ws.loop() callback)
    if (g_pendingOfflineDraw)
    {
        g_pendingOfflineDraw = false;
        drawOfflinePage();
    }

    if (g_pendingReconnectDraw)
    {
        g_pendingReconnectDraw = false;
        const int page = g_currentPage;
        g_currentPage = -1;
        switchToPage(page);
    }

    if (!g_wsConnected)
    {
        const unsigned long nowMs = millis();
        if (nowMs - g_offlineTimerLastMs >= 1000)
        {
            g_offlineTimerLastMs = nowMs;
            updateOfflineElapsed();
        }
        return;
    }

    if (g_currentPage == 0)
    {
        updateClockDisplay();
        updateDurationMetricDisplay();
        updateBerMetricDisplay();
        updateLossMetricDisplay();

        // Blink the arrow when idle
        if (strcmp(currentActivityBannerText(), "IDLE") == 0)
        {
            const unsigned long nowMs = millis();
            if (nowMs - g_idleBlinkMs >= IDLE_BLINK_MS)
            {
                g_idleBlinkMs = nowMs;
                g_idleBlinkPhase = (g_idleBlinkPhase + 1) % 14;
                g_lastActivityBannerText[0] = '\0'; // force redraw
                updateActivityStatusBannerDisplay();
            }
        }
    }
    if (touchscreen.tirqTouched() && touchscreen.touched())
    {
        TS_Point p = touchscreen.getPoint();
        x = map(p.x, 200, 3700, 1, tft.width());
        y = map(p.y, 240, 3800, 1, tft.height());
        z = p.z;

        printTouchToSerial(x, y, z);

        const unsigned long nowMs = millis();
        if (nowMs - g_lastTouchMs >= TOUCH_DEBOUNCE_MS)
        {
            g_lastTouchMs = nowMs;

            // On Static TGs page, bottom strip taps scroll instead of cycling pages
            if (g_currentPage == 2 && y > (tft.height() - 26) && g_snapshot.static_tg_count > 8)
            {
                if (x < 100)
                {
                    g_staticTgScrollOffset--;
                    drawStaticTgPage();
                }
                else if (x > 220)
                {
                    g_staticTgScrollOffset++;
                    drawStaticTgPage();
                }
            }
            else
            {
                switchToPage((g_currentPage + 1) % 4);
            }
        }

        delay(100);
    }
}
