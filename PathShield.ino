#include <M5Unified.h>
#include <NimBLEDevice.h>
#include "MacPrefixes.h"
#include <algorithm>
#include <SPIFFS.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 135
#define DEFAULT_SCREEN_TIMEOUT 30000

// Off-screen frame buffer for the screens that repaint on a timer (top bar,
// findings list, settings menu). Those used to clear the panel and then redraw
// it field by field once a second, which read as a black flash on every single
// update. They now compose into this sprite and reach the panel as one
// pushSprite() — no flicker, and fewer SPI transfers than the per-field repaint
// it replaces. 240x135 at 16bpp is ~65KB, held in PSRAM so it doesn't come out
// of the internal heap that the MEM bar reports.
//
// Screens drawn once and left up (boot splash, controls sheet, feedback toasts,
// the alert screen) never flickered, so they keep rendering straight to the
// panel rather than being churned for the sake of consistency.
//
// frame() and framePush(), which hand out the surface to draw on, are defined
// much further down next to the render functions: the Arduino preprocessor
// hoists its generated prototypes above the first function *definition* in the
// file, so defining either here would lift every prototype above the enums and
// structs they reference.
M5Canvas frameCanvas(&M5.Display);
bool canvasReady = false;

// One row of the findings list, copied out of the live device arrays so that
// drawing it doesn't need deviceMutex held — see displayTrackedDevices().
// Declared up here, above the first function definition, because Arduino's
// generated prototypes are inserted at that point and would otherwise name
// these types before they exist.
struct RowSnapshot {
  bool isWifi;
  bool detected;
  bool isSpecial;
  char name[33];  // BLE device name, or SSID
  char address[18];
  char manufacturer[31];
  uint8_t trackerType;
  float score;
  unsigned long firstSeen;
  int totalCount;
  int rssi;
  int minRssi;
  int maxRssi;
  int avgRssi;
  int channel;
  int encryptionType;
};

// Everything one frame of the findings screen needs to draw itself. Built under
// the mutex, rendered after it's released.
struct FrameSnapshot {
  RowSnapshot rows[3];
  int rowsFilled;
  int totalItems;
  int scrollIndex;
  bool wifiView;
  bool merged;
  bool alertsOnly;
  bool nothingTracked;
  int bleTracked;
  int wifiTracked;
};

// Single source of truth for the version. Shown on the boot splash, in the
// settings header, and over serial at boot, so you can confirm on-device which
// build is actually running. Keep this in step with docs/manifest.json —
// including the cache-busting `?v=` on its firmware paths, or the web flasher
// will happily reinstall a stale binary from CDN cache.
#define FIRMWARE_VERSION "2.4.1"

// M5StickS3 always has 8MB PSRAM, so device limits are fixed at boot.
#define MAX_DEVICES_CAP 70
#define MAX_WIFI_DEVICES_CAP 50
#define DETECTION_WINDOW 300
#define IDLE_TIMEOUT 30000
#define MAX_TIMESTAMPS 20

int maxDevices = MAX_DEVICES_CAP;
int maxWifiDevices = MAX_WIFI_DEVICES_CAP;
bool hasPsram = false;

#define BLUE_GREY 0x5D9B

#define WIFI_NAME_COLOR GREEN
#define BLE_NAME_COLOR  CYAN

#define WINDOW_RECENT 300
#define WINDOW_MEDIUM 600
#define WINDOW_OLD 900
#define WINDOW_OLDEST 1200

#define MIN_DETECTIONS 12
#define MIN_WINDOWS 3
#define EPSILON_CONNECTED_GAP 180
#define MIN_RSSI_RANGE 12
#define RSSI_FLOOR -80

// The three sensitivity knobs that most directly affect false positive/negative
// rate. Runtime-configurable via the serial "threshold" command and persisted to
// /prefs.txt — the rest of the scoring constants above stay compile-time, since
// they're the algorithm's internal weighting rather than something to safely
// hand-tune device-side.
#define DEFAULT_PERSISTENCE_THRESHOLD 0.75f
#define DEFAULT_RSSI_STABILITY_THRESHOLD 10
#define DEFAULT_RSSI_VARIATION_THRESHOLD 15
float persistenceThreshold = DEFAULT_PERSISTENCE_THRESHOLD;
int rssiStabilityThreshold = DEFAULT_RSSI_STABILITY_THRESHOLD;
int rssiVariationThreshold = DEFAULT_RSSI_VARIATION_THRESHOLD;

const int MEMORY_CRITICAL = 50;
const int MEMORY_WARNING = 80;
const int MEMORY_GOOD = 150;

// Rough linear estimate, not based on real current draw — matches the
// ~4-6h continuous dual-band scanning range documented in the README.
#define TYPICAL_BATTERY_LIFE_HOURS 5.0f

unsigned long lastBtnAPress = 0;
unsigned long lastBtnBPress = 0;
const unsigned long DEBOUNCE_DELAY = 200;
bool inMenu = false;
int menuIndex = 0;
int menuBaseY = 0;
#define MENU_OPTION_COUNT 10
// 9px rows fit ten options plus the status line, divider and footer hint in
// 135px; the 8px font leaves a 1px gap. At 10px the tenth row ran off screen.
#define MENU_ROW_H 9

// How long a button must stay down to count as a hold rather than a tap.
// Every hold gesture uses this one value so "hold" means the same thing
// everywhere — see waitForHold().
#define BUTTON_HOLD_MS 600

bool highBrightness = true;
bool currentlyBright = true;
bool paused = false;
enum FilterMode { FILTER_ALL, FILTER_NAMED, FILTER_ALERTS };
FilterMode filterMode = FILTER_ALL;

// Visual intensity of a tracker alert. SIMPLE is the default: the old
// red/blue strobe (now LOUD) is deliberately opt-in, since a flashing screen
// is a lot to inflict on someone every time a tracker turns up.
enum AlertStyle : uint8_t { ALERT_LOUD = 0, ALERT_SIMPLE = 1, ALERT_QUIET = 2 };
AlertStyle alertStyle = ALERT_SIMPLE;
bool alertSoundEnabled = true;

// How signal strength reads on the findings list: a four-bar glyph, or the raw
// dBm figure. Bars are quicker to judge at a glance ("is it getting closer?"),
// dBm is what you want when comparing two devices or reporting a finding — so
// this is a preference rather than a replacement. Persisted to /prefs.txt.
bool showSignalBars = true;

const char *alertStyleName(AlertStyle style) {
  switch (style) {
    case ALERT_LOUD:  return "LOUD";
    case ALERT_QUIET: return "QUIET";
    default:          return "SIMPLE";
  }
}

bool screenDimmed = false;
unsigned long lastButtonPressTime = 0;
unsigned long lastActivityTime = 0;
bool screenOn = true;
unsigned long screenTimeoutMs = DEFAULT_SCREEN_TIMEOUT;
static uint32_t lastStateHash = 0;
unsigned long lastDisplayRender = 0;
unsigned long lastMenuRender = 0;

// How long a tracker alert stays up before standing down on its own. Long
// enough to read the type, name, MAC and score off the screen; short enough
// that an alert nobody is present to acknowledge doesn't keep the scanner
// parked. A button press still dismisses it immediately.
const unsigned long ALERT_DURATION = 8000;

// Alerts that timed out instead of being acknowledged by hand. The findings
// screen keeps a red border and an !N badge up while this is non-zero, so an
// alert that fired while the device was in a pocket still leaves a trace.
// Cleared by looking at the ALERTS filter.
int unackedAlertCount = 0;

// Alert handoff from scanTask (Core 0) to loop() (Core 1).
// All M5.Display / M5.update() calls must happen on Core 1 only — scanTask
// never touches display or button hardware directly, it just deposits data here.
struct PendingAlertInfo {
  bool isSpecial;
  char name[21];
  char mac[18];
  float score;
  uint8_t trackerType;
  // Which band found it. The alert screen labels the identifier accordingly —
  // "BSSID" is not a MAC the BLE allowlist could ever act on, and saying so
  // saves the reader working out why.
  bool isWifi;
};
PendingAlertInfo pendingAlertInfo;
volatile bool alertPending = false;
volatile bool showingAlert = false;

struct WiFiDeviceInfo {
  char ssid[33];
  char bssid[18];
  int rssi;
  int channel;
  int encryptionType;
  unsigned long lastSeen;
  int detectionCount;
  // Matched a specialMacs[] prefix. There's no persistence scoring on the WiFi
  // side — a privacy-invader OUI is an immediate hit, the same way it is for
  // BLE — so these two carry all the alert state a WiFi entry needs.
  bool isSpecial;
  bool alertTriggered;
};

WiFiDeviceInfo *wifiDevices = NULL;
int wifiDeviceIndex = 0;
bool scanningWiFi = true;
unsigned long lastScanSwitch = 0;
const unsigned long SCAN_SWITCH_INTERVAL = 3000;

// Privacy Invader Defaults: Axon cameras, Liteon Technology (Flock), Utility Inc (Flock) OUIs.
// Runtime-configurable via the serial "special" command (see handleSpecialCommand())
// and persisted to /specialmacs.txt — falls back to these compiled-in defaults
// whenever that file doesn't exist yet (first boot, or after a "special reset").
#define MAX_SPECIAL_MACS 20
const char *defaultSpecialMacs[] = {"00:25:DF", "14:5A:FC", "00:09:BC"};
char specialMacs[MAX_SPECIAL_MACS][18];
int specialMacsCount = 0;

// BLE Tracker Type Detection
enum TrackerType : uint8_t {
  TRACKER_NONE = 0,
  TRACKER_AIRTAG,
  TRACKER_TILE,
  TRACKER_SMARTTAG,
  TRACKER_CHIPOLO,
  TRACKER_GOOGLE_FMDN,
  TRACKER_PRIVACY_INVADER,
  TRACKER_PERSISTENCE
};

const char* trackerTypeName(uint8_t type) {
  switch (type) {
    case TRACKER_AIRTAG:         return "AirTag";
    case TRACKER_TILE:           return "Tile";
    case TRACKER_SMARTTAG:       return "SmartTag";
    case TRACKER_CHIPOLO:        return "Chipolo";
    case TRACKER_GOOGLE_FMDN:    return "Google FMDN";
    case TRACKER_PRIVACY_INVADER:return "Privacy Inv";
    case TRACKER_PERSISTENCE:    return "Persistence";
    default:                     return "Unknown";
  }
}

const char *wifiAuthName(int encryptionType) {
  switch (encryptionType) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI";
    case WIFI_AUTH_OWE:             return "OWE";
    case WIFI_AUTH_WPA3_ENT_192:    return "WPA3-E";
    default:                        return "UNK";
  }
}

void formatDuration(unsigned long seconds, char *out, size_t outSize) {
  if (seconds < 60) {
    snprintf(out, outSize, "<1m");
  } else if (seconds < 3600) {
    snprintf(out, outSize, "%lum", seconds / 60);
  } else {
    snprintf(out, outSize, "%luh%02lum", seconds / 3600, (seconds % 3600) / 60);
  }
}

// IGNORE LIST: Add MAC prefixes of YOUR devices here to ignore them
// Example: const char *allowlistMacs[] = {"AA:BB:CC", "DD:EE:FF", "11:22:33"};
// Leave as {""} to track all devices
const char *allowlistMacs[] = {""};

// Runtime allowlist — devices added in the field (hold Button A on the
// paused findings list) rather than compiled in. Exact full-MAC match, not
// an OUI prefix like allowlistMacs[] above: a quick in-field action should
// suppress the one device you're looking at, not silently widen to every
// device from that manufacturer. Persisted to /allowlist.txt.
#define MAX_RUNTIME_ALLOWLIST 20
char runtimeAllowlist[MAX_RUNTIME_ALLOWLIST][18];
int runtimeAllowlistCount = 0;

// Topmost device on the currently-rendered paused BLE list, cached by
// displayTrackedDevices() so handleBtnA()'s hold-to-allowlist gesture acts on
// exactly what the user is looking at.
char topVisibleAddress[18] = "";
bool topVisibleValid = false;

struct TimeWindow {
  unsigned long start;
  unsigned long end;
  int detections;
};

struct DeviceInfo {
  char address[18];
  char name[21];
  char manufacturer[31];
  int totalCount;
  unsigned long firstSeen;
  unsigned long lastSeen;
  int rssiSum;
  int rssiCount;
  int lastRssi;
  int minRssi;
  int maxRssi;
  bool detected;
  bool isSpecial;
  bool alertTriggered;
  int stableRssiCount;
  int variationCount;
  float persistenceScore;
  TimeWindow windows[4];
  unsigned long timestamps[MAX_TIMESTAMPS];
  uint8_t tsIdx;
  uint8_t tsCount;
  uint8_t trackerType;
};

DeviceInfo *trackedDevices = NULL;
int deviceIndex = 0;
// Number of rows the last render actually put on screen (post-filter), and
// which of the two lists it drew. Scrolling and the WiFi/BLE label both key
// off what's *displayed*, not off whichever band scanTask happens to be on.
int lastVisibleItemCount = 0;
bool displayingWifiView = true;
void updateTimeWindows(DeviceInfo &device, unsigned long currentTime);
float calculatePersistenceScore(DeviceInfo &device, unsigned long currentTime);
NimBLEScan *pBLEScan;
int scrollIndex = 0;

// Known device ring buffer — compact storage for stable-RSSI devices
#define MAX_KNOWN_CAP 80
#define KNOWN_PROMOTE_COUNT 8
#define KNOWN_RSSI_DRIFT 20
#define KNOWN_EXPIRY_WINDOW (DETECTION_WINDOW * 2)

struct KnownDevice {
  char address[18];
  unsigned long lastSeen;
  int avgRssi;
  uint8_t seenCount;
};

KnownDevice *knownDevices = NULL;
int knownDeviceCount = 0;
int maxKnownDevices = MAX_KNOWN_CAP;

SemaphoreHandle_t deviceMutex = NULL;
TaskHandle_t scanTaskHandle = NULL;
volatile bool scanTaskRunning = true;
uint32_t initialFreeHeapKB = 40;  // Set after allocations in setup()

class MyScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {}
};

// Case-insensitive, and it has to be: NimBLE renders addresses lowercase
// ("%02x") while WiFi.BSSIDstr() renders them uppercase ("%02X"), and the
// prefixes these are matched against are written uppercase — in
// defaultSpecialMacs[], in allowlistMacs[], in the README, and in whatever a
// user types at the `special add` / `allow add` serial commands. A plain
// strncmp therefore never matched a BLE address against any prefix containing
// a hex letter, which is all three shipped defaults. getManufacturer() in
// MacPrefixes.h already normalises case for exactly this reason.
bool isSpecialMac(const char *address) {
  for (int i = 0; i < specialMacsCount; i++) {
    if (strncasecmp(address, specialMacs[i], strlen(specialMacs[i])) == 0) {
      return true;
    }
  }
  return false;
}

bool isAllowlistedMac(const char *address) {
  for (size_t i = 0; i < sizeof(allowlistMacs) / sizeof(allowlistMacs[0]); i++) {
    if (strlen(allowlistMacs[i]) > 0 &&
        strncasecmp(address, allowlistMacs[i], strlen(allowlistMacs[i])) == 0) {
      return true;
    }
  }
  for (int i = 0; i < runtimeAllowlistCount; i++) {
    if (strcasecmp(address, runtimeAllowlist[i]) == 0) {
      return true;
    }
  }
  return false;
}

// Detect known BLE tracker types from advertisement data
uint8_t detectTrackerType(const NimBLEAdvertisedDevice &device) {
  // Check manufacturer data (company ID is first 2 bytes, little-endian)
  if (device.haveManufacturerData()) {
    std::string mfgData = device.getManufacturerData();
    if (mfgData.length() >= 2) {
      uint16_t companyId = (uint8_t)mfgData[0] | ((uint8_t)mfgData[1] << 8);

      // Apple AirTag: company ID 0x004C with 25+ bytes of payload after company ID
      // Also catches Find My network accessories (Chipolo ONE Spot, Eufy in FM mode)
      if (companyId == 0x004C && mfgData.length() >= 27) {
        // AirTag/Find My devices have specific payload type byte at offset 2
        uint8_t payloadType = (uint8_t)mfgData[2];
        if (payloadType == 0x12 || payloadType == 0x07) {
          return TRACKER_AIRTAG;
        }
      }

      // Samsung SmartTag: company ID 0x0075
      if (companyId == 0x0075) {
        return TRACKER_SMARTTAG;
      }

      // Chipolo (native mode, not Find My): company ID 0x0133
      if (companyId == 0x0133) {
        return TRACKER_CHIPOLO;
      }

      // Google Find My Device Network: company ID 0x00E0
      if (companyId == 0x00E0) {
        return TRACKER_GOOGLE_FMDN;
      }
    }
  }

  // Check service UUIDs
  if (device.haveServiceUUID()) {
    // Tile: service UUID 0xFD51 or 0xFD52
    if (device.isAdvertisingService(NimBLEUUID((uint16_t)0xFD51)) ||
        device.isAdvertisingService(NimBLEUUID((uint16_t)0xFD52))) {
      return TRACKER_TILE;
    }

    // Samsung SmartThings/Find: service UUID 0xFD6F
    if (device.isAdvertisingService(NimBLEUUID((uint16_t)0xFD6F))) {
      return TRACKER_SMARTTAG;
    }
  }

  // Check device name as fallback
  std::string devName = device.getName();
  if (devName.length() > 0) {
    // Convert to lowercase for comparison
    char nameLower[21];
    size_t len = std::min(devName.length(), (size_t)20);
    for (size_t i = 0; i < len; i++) {
      nameLower[i] = tolower(devName[i]);
    }
    nameLower[len] = '\0';

    if (strstr(nameLower, "tile"))     return TRACKER_TILE;
    if (strstr(nameLower, "smarttag")) return TRACKER_SMARTTAG;
    if (strstr(nameLower, "chipolo"))  return TRACKER_CHIPOLO;
  }

  return TRACKER_NONE;
}

// Returns true when this sighting is a *new* privacy-invader hit the caller
// should raise an alert for. isSpecialMac() was previously only ever consulted
// on the BLE path, so the Axon and Flock OUIs that ship in defaultSpecialMacs[]
// never fired over WiFi — despite the README billing them as "Privacy Invader
// Defaults" and the web flasher's threat matrix listing them as WiFi/BLE. Flock
// cameras beacon over WiFi. There's no persistence scoring here: a matching OUI
// is an immediate hit, the same as it is for BLE.
bool trackWiFiDevice(const char *ssid, const char *bssid, int rssi, int channel,
                     int encType, unsigned long currentTime) {
  bool special = isSpecialMac(bssid);

  for (int i = 0; i < wifiDeviceIndex; i++) {
    if (strcasecmp(wifiDevices[i].bssid, bssid) == 0) {
      wifiDevices[i].rssi = rssi;
      wifiDevices[i].lastSeen = currentTime;
      wifiDevices[i].detectionCount++;
      wifiDevices[i].isSpecial = special;
      if (special && !wifiDevices[i].alertTriggered) {
        wifiDevices[i].alertTriggered = true;
        return true;
      }
      return false;
    }
  }

  if (wifiDeviceIndex < maxWifiDevices) {
    strncpy(wifiDevices[wifiDeviceIndex].ssid, ssid, 32);
    wifiDevices[wifiDeviceIndex].ssid[32] = '\0';
    strncpy(wifiDevices[wifiDeviceIndex].bssid, bssid, 17);
    wifiDevices[wifiDeviceIndex].bssid[17] = '\0';
    wifiDevices[wifiDeviceIndex].rssi = rssi;
    wifiDevices[wifiDeviceIndex].channel = channel;
    wifiDevices[wifiDeviceIndex].encryptionType = encType;
    wifiDevices[wifiDeviceIndex].lastSeen = currentTime;
    wifiDevices[wifiDeviceIndex].detectionCount = 1;
    wifiDevices[wifiDeviceIndex].isSpecial = special;
    wifiDevices[wifiDeviceIndex].alertTriggered = special;
    wifiDeviceIndex++;
    return special;
  } else {
    int oldestIndex = -1;
    unsigned long oldestTime = currentTime;

    // Never evict an alerting entry to make room for a routine one — it's the
    // one row on this list that matters.
    for (int i = 0; i < wifiDeviceIndex; i++) {
      if (wifiDevices[i].alertTriggered) continue;
      if (wifiDevices[i].lastSeen < oldestTime) {
        oldestTime = wifiDevices[i].lastSeen;
        oldestIndex = i;
      }
    }

    if (oldestIndex != -1) {
      strncpy(wifiDevices[oldestIndex].ssid, ssid, 32);
      wifiDevices[oldestIndex].ssid[32] = '\0';
      strncpy(wifiDevices[oldestIndex].bssid, bssid, 17);
      wifiDevices[oldestIndex].bssid[17] = '\0';
      wifiDevices[oldestIndex].rssi = rssi;
      wifiDevices[oldestIndex].channel = channel;
      wifiDevices[oldestIndex].encryptionType = encType;
      wifiDevices[oldestIndex].lastSeen = currentTime;
      wifiDevices[oldestIndex].detectionCount = 1;
      wifiDevices[oldestIndex].isSpecial = special;
      wifiDevices[oldestIndex].alertTriggered = special;
      return special;
    }
  }

  return false;
}

void removeOldWiFiEntries(unsigned long currentTime) {
  for (int i = 0; i < wifiDeviceIndex; i++) {
    // Alerting entries are kept, matching how removeOldEntries() preserves
    // alertTriggered BLE devices — an alert you walked out of range of is still
    // the thing you want on screen when you look down.
    if (wifiDevices[i].alertTriggered) continue;

    if (currentTime - wifiDevices[i].lastSeen > DETECTION_WINDOW) {
      for (int j = i; j < wifiDeviceIndex - 1; j++) {
        wifiDevices[j] = wifiDevices[j + 1];
      }
      wifiDeviceIndex--;
      i--;
    }
  }
}

void updateTimeWindows(DeviceInfo &device, unsigned long currentTime) {
  device.windows[0].start =
      currentTime >= WINDOW_RECENT ? currentTime - WINDOW_RECENT : 0;
  device.windows[0].end = currentTime;
  device.windows[1].start =
      currentTime >= WINDOW_MEDIUM ? currentTime - WINDOW_MEDIUM : 0;
  device.windows[1].end = device.windows[0].start;
  device.windows[2].start =
      currentTime >= WINDOW_OLD ? currentTime - WINDOW_OLD : 0;
  device.windows[2].end = device.windows[1].start;
  device.windows[3].start =
      currentTime >= WINDOW_OLDEST ? currentTime - WINDOW_OLDEST : 0;
  device.windows[3].end = device.windows[2].start;

  for (int w = 0; w < 4; w++) {
    device.windows[w].detections = 0;
    for (int i = 0; i < device.tsCount; i++) {
      if (device.timestamps[i] >= device.windows[w].start &&
          device.timestamps[i] < device.windows[w].end) {
        device.windows[w].detections++;
      }
    }
  }
}

float calculatePersistenceScore(DeviceInfo &device, unsigned long currentTime) {
  float score = 0.0f;
  int rssiRange = device.maxRssi - device.minRssi;

  if (rssiRange < MIN_RSSI_RANGE) {
    return 0.0f;
  }

  if (device.totalCount >= MIN_DETECTIONS) {
    score += 0.20f * (float)std::min(device.totalCount, 30) / 30.0f;
  } else {
    return 0.0f;
  }

  int windowsActive = 0;
  for (int i = 0; i < 4; i++) {
    if (device.windows[i].detections > 0) windowsActive++;
  }
  if (windowsActive >= MIN_WINDOWS) {
    score += 0.25f * (float)windowsActive / 4.0f;
  } else {
    return 0.0f;
  }

  bool isConnected = true;
  if (device.tsCount >= 3) {
    for (int i = 1; i < device.tsCount; i++) {
      int prev =
          (device.tsIdx - device.tsCount + i - 1 + MAX_TIMESTAMPS) % MAX_TIMESTAMPS;
      int curr =
          (device.tsIdx - device.tsCount + i + MAX_TIMESTAMPS) % MAX_TIMESTAMPS;
      if (device.timestamps[curr] - device.timestamps[prev] >
          EPSILON_CONNECTED_GAP) {
        isConnected = false;
        break;
      }
    }
    if (isConnected) {
      score += 0.25f;
    }
  }

  if (device.variationCount >= 3) { // sketchy variance
    score += 0.30f * (float)std::min(device.variationCount, 10) / 10.0f;
  }

  return score;
}

void moveToTop(int index) {
  if (index <= 0) return;
  DeviceInfo temp = trackedDevices[index];
  for (int i = index; i > 0; i--) {
    trackedDevices[i] = trackedDevices[i - 1];
  }
  trackedDevices[0] = temp;
}

int findEvictionCandidate() {
  int bestCandidate = -1;
  float minScore = 100.0f;
  unsigned long oldestSeen = 0xFFFFFFFF;

  for (int i = 0; i < deviceIndex; i++) {
    if (trackedDevices[i].isSpecial || trackedDevices[i].alertTriggered) continue;

    if (trackedDevices[i].persistenceScore < minScore) {
      minScore = trackedDevices[i].persistenceScore;
      bestCandidate = i;
      oldestSeen = trackedDevices[i].lastSeen;
    } else if (trackedDevices[i].persistenceScore == minScore) {
      if (trackedDevices[i].lastSeen < oldestSeen) {
        bestCandidate = i;
        oldestSeen = trackedDevices[i].lastSeen;
      }
    }
  }

  if (bestCandidate == -1 && deviceIndex > 0) {
    oldestSeen = 0xFFFFFFFF;
    for (int i = 0; i < deviceIndex; i++) {
      if (trackedDevices[i].lastSeen < oldestSeen) {
        bestCandidate = i;
        oldestSeen = trackedDevices[i].lastSeen;
      }
    }
  }

  return bestCandidate;
}

bool trackDevice(const char *address, int rssi, unsigned long currentTime,
                 const char *name, uint8_t detectedTracker = TRACKER_NONE) {
  bool newTracker = false;

  for (int i = 0; i < deviceIndex; i++) {
    if (strcmp(trackedDevices[i].address, address) == 0) {
      // Update tracker type if newly identified
      if (detectedTracker != TRACKER_NONE && trackedDevices[i].trackerType == TRACKER_NONE) {
        trackedDevices[i].trackerType = detectedTracker;
      }
      trackedDevices[i].totalCount++;
      trackedDevices[i].lastSeen = currentTime;
      trackedDevices[i].rssiSum += rssi;
      trackedDevices[i].rssiCount++;

      // Track RSSI range for movement detection
      if (rssi < trackedDevices[i].minRssi) trackedDevices[i].minRssi = rssi;
      if (rssi > trackedDevices[i].maxRssi) trackedDevices[i].maxRssi = rssi;

      trackedDevices[i].timestamps[trackedDevices[i].tsIdx] = currentTime;
      trackedDevices[i].tsIdx =
          (trackedDevices[i].tsIdx + 1) % MAX_TIMESTAMPS;
      if (trackedDevices[i].tsCount < MAX_TIMESTAMPS)
        trackedDevices[i].tsCount++;

      int rssiDiff = abs(trackedDevices[i].lastRssi - rssi);
      if (rssiDiff <= rssiStabilityThreshold) {
        trackedDevices[i].stableRssiCount++;
      } else {
        trackedDevices[i].stableRssiCount = 0;
      }
      if (rssiDiff >= rssiVariationThreshold) {
        trackedDevices[i].variationCount++;
      }
      trackedDevices[i].lastRssi = rssi;

      if (name && strlen(name) > 0) {
        strncpy(trackedDevices[i].name, name, 20);
        trackedDevices[i].name[20] = '\0';
      }

      getManufacturer(address, trackedDevices[i].manufacturer, 31);

      updateTimeWindows(trackedDevices[i], currentTime);
      trackedDevices[i].persistenceScore =
          calculatePersistenceScore(trackedDevices[i], currentTime);

      if (isSpecialMac(address)) {
        trackedDevices[i].detected = true;
        trackedDevices[i].isSpecial = true;
        if (trackedDevices[i].trackerType == TRACKER_NONE)
          trackedDevices[i].trackerType = TRACKER_PRIVACY_INVADER;
        if (!trackedDevices[i].alertTriggered) {
          trackedDevices[i].alertTriggered = true;
          newTracker = true;
        }
        moveToTop(i);
      } else if (trackedDevices[i].trackerType != TRACKER_NONE) {
        // Known tracker type detected via BLE signature — immediate alert
        trackedDevices[i].detected = true;
        trackedDevices[i].isSpecial = true;
        if (!trackedDevices[i].alertTriggered) {
          trackedDevices[i].alertTriggered = true;
          newTracker = true;
        }
        moveToTop(i);
      } else if (trackedDevices[i].persistenceScore >= persistenceThreshold) {
        trackedDevices[i].detected = true;
        trackedDevices[i].isSpecial = false;
        trackedDevices[i].trackerType = TRACKER_PERSISTENCE;
        if (!trackedDevices[i].alertTriggered) {
          trackedDevices[i].alertTriggered = true;
          newTracker = true;
        }
        moveToTop(i);
      }
      return newTracker;
    }
  }

  int targetIndex = -1;

  if (deviceIndex < maxDevices) {
    for (int j = deviceIndex; j > 0; j--) {
      trackedDevices[j] = trackedDevices[j - 1];
    }
    targetIndex = 0;
    deviceIndex++;
  } else {
    int evictIdx = findEvictionCandidate();
    if (evictIdx != -1) {
      for (int j = evictIdx; j > 0; j--) {
        trackedDevices[j] = trackedDevices[j - 1];
      }
      targetIndex = 0;
    }
  }

  if (targetIndex == 0) {
    memset(&trackedDevices[0], 0, sizeof(DeviceInfo));
    strncpy(trackedDevices[0].address, address, 17);
    trackedDevices[0].address[17] = '\0';

    if (name && strlen(name) > 0) {
      strncpy(trackedDevices[0].name, name, 20);
      trackedDevices[0].name[20] = '\0';
    }

    getManufacturer(address, trackedDevices[0].manufacturer, 31);

    trackedDevices[0].totalCount = 1;
    trackedDevices[0].firstSeen = currentTime;
    trackedDevices[0].lastSeen = currentTime;
    trackedDevices[0].rssiSum = rssi;
    trackedDevices[0].rssiCount = 1;
    trackedDevices[0].lastRssi = rssi;
    trackedDevices[0].minRssi = rssi;
    trackedDevices[0].maxRssi = rssi;
    trackedDevices[0].timestamps[0] = currentTime;
    trackedDevices[0].tsIdx = 1;
    trackedDevices[0].tsCount = 1;
    trackedDevices[0].trackerType = detectedTracker;

    if (isSpecialMac(address)) {
      trackedDevices[0].detected = true;
      trackedDevices[0].isSpecial = true;
      trackedDevices[0].alertTriggered = true;
      if (trackedDevices[0].trackerType == TRACKER_NONE)
        trackedDevices[0].trackerType = TRACKER_PRIVACY_INVADER;
      newTracker = true;
    } else if (detectedTracker != TRACKER_NONE) {
      // Known tracker type on first sight — immediate alert
      trackedDevices[0].detected = true;
      trackedDevices[0].isSpecial = true;
      trackedDevices[0].alertTriggered = true;
      newTracker = true;
    }
  }

  return newTracker;
}

void removeOldEntries(unsigned long currentTime) {
  for (int i = 0; i < deviceIndex; i++) {
    if (trackedDevices[i].alertTriggered) continue;

    if (currentTime - trackedDevices[i].lastSeen > DETECTION_WINDOW) {
      for (int j = i; j < deviceIndex - 1; j++) {
        trackedDevices[j] = trackedDevices[j + 1];
      }
      deviceIndex--;
      i--;
    }
  }

  // Promote stable devices to known buffer
  for (int i = 0; i < deviceIndex; i++) {
    if (trackedDevices[i].alertTriggered || trackedDevices[i].isSpecial) continue;
    if (trackedDevices[i].totalCount >= KNOWN_PROMOTE_COUNT &&
        (trackedDevices[i].maxRssi - trackedDevices[i].minRssi) < MIN_RSSI_RANGE) {
      promoteToKnown(i, currentTime);
      i--;
    }
  }
}

// --- Known device ring buffer functions ---

int findKnownDevice(const char *address) {
  for (int i = 0; i < knownDeviceCount; i++) {
    if (strcmp(knownDevices[i].address, address) == 0) return i;
  }
  return -1;
}

void promoteToKnown(int trackedIdx, unsigned long currentTime) {
  int ki = findKnownDevice(trackedDevices[trackedIdx].address);
  if (ki == -1) {
    // Find slot: use next empty or evict oldest
    if (knownDeviceCount < maxKnownDevices) {
      ki = knownDeviceCount++;
    } else {
      ki = 0;
      unsigned long oldest = knownDevices[0].lastSeen;
      for (int i = 1; i < knownDeviceCount; i++) {
        if (knownDevices[i].lastSeen < oldest) {
          oldest = knownDevices[i].lastSeen;
          ki = i;
        }
      }
    }
  }

  strncpy(knownDevices[ki].address, trackedDevices[trackedIdx].address, 17);
  knownDevices[ki].address[17] = '\0';
  knownDevices[ki].lastSeen = currentTime;
  knownDevices[ki].avgRssi = trackedDevices[trackedIdx].rssiSum /
                              trackedDevices[trackedIdx].rssiCount;
  knownDevices[ki].seenCount = (uint8_t)std::min(trackedDevices[trackedIdx].totalCount, 255);

  // Remove from tracked array
  for (int j = trackedIdx; j < deviceIndex - 1; j++) {
    trackedDevices[j] = trackedDevices[j + 1];
  }
  deviceIndex--;
}

// Returns true if device was handled as known (caller should skip trackDevice)
bool handleKnownDevice(const char *address, int rssi, unsigned long currentTime) {
  int ki = findKnownDevice(address);
  if (ki == -1) return false;

  // Check if RSSI drifted significantly — device or user is moving
  if (abs(rssi - knownDevices[ki].avgRssi) > KNOWN_RSSI_DRIFT) {
    // De-promote: remove from known, let it enter full tracking
    knownDevices[ki] = knownDevices[knownDeviceCount - 1];
    knownDeviceCount--;
    return false;
  }

  // Still stable — just update and skip full tracking
  knownDevices[ki].lastSeen = currentTime;
  if (knownDevices[ki].seenCount < 255) knownDevices[ki].seenCount++;
  // Exponential moving average for RSSI
  knownDevices[ki].avgRssi = (knownDevices[ki].avgRssi * 3 + rssi) / 4;
  return true;
}

void removeOldKnownEntries(unsigned long currentTime) {
  for (int i = 0; i < knownDeviceCount; i++) {
    if (currentTime - knownDevices[i].lastSeen > KNOWN_EXPIRY_WINDOW) {
      knownDevices[i] = knownDevices[knownDeviceCount - 1];
      knownDeviceCount--;
      i--;
    }
  }
}

void alertUser(bool isSpecial, const char *name, const char *mac,
               float persistence, uint8_t tType = TRACKER_NONE,
               bool isWifi = false) {

  screenOn = true;
  lastActivityTime = millis();
  // Loud/Simple jump to max brightness to grab attention. Quiet respects
  // whatever brightness the user already chose — the point is to not draw
  // attention, so don't force the screen bright either.
  M5.Display.setBrightness(alertStyle == ALERT_QUIET ? (highBrightness ? 204 : 77) : 204);

  if (alertSoundEnabled) {
    M5.Speaker.tone(1800, 120);
    delay(160);
    M5.Speaker.tone(1800, 120);
    delay(160);
  }

  if (alertStyle == ALERT_QUIET) {
    // No strobe, no fill: a black screen with a thin colored border — enough
    // to notice without drawing attention to the device.
    M5.Display.fillScreen(BLACK);
    M5.Display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, isSpecial ? ORANGE : RED);
    M5.Display.drawRect(1, 1, SCREEN_WIDTH - 2, SCREEN_HEIGHT - 2, isSpecial ? ORANGE : RED);
  } else if (alertStyle == ALERT_LOUD && isSpecial) {
    for (int i = 0; i < 5; i++) {
      M5.Display.fillScreen(RED);
      delay(200);
      M5.Display.fillScreen(BLUE);
      delay(200);
    }
  } else {
    // Simple: one solid red screen, no animation. Privacy-invader hits still
    // read differently at a glance via an orange border rather than a strobe.
    M5.Display.fillScreen(RED);
    if (isSpecial) {
      for (int i = 0; i < 3; i++) {
        M5.Display.drawRect(i, i, SCREEN_WIDTH - i * 2, SCREEN_HEIGHT - i * 2, ORANGE);
      }
    }
  }

  M5.Display.setCursor(0, 10);
  M5.Display.setTextColor(alertStyle == ALERT_QUIET ? (isSpecial ? ORANGE : RED) : WHITE);
  M5.Display.setTextSize(2);
  M5.Display.print(alertStyle == ALERT_QUIET ? "Tracker Alert" : "Tracker Detected!");
  M5.Display.setTextSize(1);

  M5.Display.setCursor(0, 40);
  M5.Display.print("Type: ");
  if (tType != TRACKER_NONE) {
    M5.Display.setTextColor(RED);
    M5.Display.print(trackerTypeName(tType));
  } else {
    M5.Display.print(isSpecial ? "KNOWN" : "SUSPECTED");
  }
  M5.Display.setTextColor(WHITE);

  M5.Display.setCursor(0, 55);
  M5.Display.print(isWifi ? "SSID: " : "Name: ");
  M5.Display.print(strlen(name) > 0 ? name : "Unknown");

  M5.Display.setCursor(0, 70);
  M5.Display.print(isWifi ? "BSSID: " : "MAC: ");
  M5.Display.print(mac);

  M5.Display.setCursor(0, 85);
  // A WiFi hit is an OUI match, not a persistence score — printing "Score:
  // 1.00" would imply a calculation that never ran.
  if (isWifi) {
    M5.Display.print("Band: WiFi Ch/OUI match");
  } else {
    M5.Display.print("Score: ");
    M5.Display.print(persistence, 2);
  }

  // Bounded wait. This used to block on a button press with no timeout, and
  // scanTask parks on showingAlert until it returns — so the moment PathShield
  // found something was the moment it stopped looking, indefinitely if the
  // device was in a pocket. For an anti-stalking tool that is exactly
  // backwards. The alert now stands down on its own and scanning resumes; the
  // countdown says so, and an unacknowledged alert leaves the findings screen
  // marked rather than vanishing silently.
  uint16_t bgColor = (alertStyle == ALERT_QUIET) ? BLACK : RED;
  unsigned long alertShownAt = millis();
  bool acknowledged = false;
  int lastSecondsShown = -1;

  while (true) {
    unsigned long elapsed = millis() - alertShownAt;
    if (elapsed >= ALERT_DURATION) break;

    M5.update();
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) {
      acknowledged = true;
      break;
    }

    int secondsLeft = (int)((ALERT_DURATION - elapsed + 999) / 1000);
    if (secondsLeft != lastSecondsShown) {
      lastSecondsShown = secondsLeft;
      // Inset so repainting the countdown doesn't eat the Quiet-mode border
      // (or the special-device orange frame) running down either edge.
      M5.Display.fillRect(3, 105, SCREEN_WIDTH - 6, 10, bgColor);
      M5.Display.setCursor(3, 105);
      M5.Display.setTextColor(YELLOW);
      M5.Display.printf("Any button  (%ds)", secondsLeft);
    }

    delay(50);
  }

  if (!acknowledged && unackedAlertCount < 99) {
    unackedAlertCount++;
  }

  // Force display reset after alert dismissal to prevent race condition
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE);
  M5.Display.setTextSize(1);
  lastStateHash = 0;
  screenOn = true;
  lastActivityTime = millis();
  M5.Display.setBrightness(highBrightness ? 204 : 77);
}

void showFeedback(const char* msg, uint16_t color, const char* sub = NULL) {
  M5.Display.fillScreen(BLACK);
  for (int i = 0; i < 15; i++) {
    M5.Display.drawFastHLine(random(0, SCREEN_WIDTH), random(0, SCREEN_HEIGHT),
                             random(5, 40), random(0x0000, 0x1111));
  }
  M5.Display.drawFastHLine(0, 35, SCREEN_WIDTH, CYAN);
  M5.Display.drawFastHLine(0, 36, SCREEN_WIDTH, CYAN);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(color);
  int xPos = (SCREEN_WIDTH - strlen(msg) * 18) / 2;
  M5.Display.setCursor(xPos, 50);
  M5.Display.print(msg);
  if (sub) {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(YELLOW);
    M5.Display.setCursor((SCREEN_WIDTH - strlen(sub) * 6) / 2, 80);
    M5.Display.print(sub);
  }
  M5.Display.drawFastHLine(0, 95, SCREEN_WIDTH, MAGENTA);
  M5.Display.drawFastHLine(0, 96, SCREEN_WIDTH, MAGENTA);
}

// The full control scheme on one screen. Shown once at boot and available any
// time from the settings menu — six gestures across two modes is more than a
// 240x135 footer hint can carry, and "which button opens the menu" shouldn't
// be something anyone has to guess at.
void drawControlsScreen() {
  M5.Display.fillScreen(BLACK);

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(CYAN);
  M5.Display.setCursor(2, 2);
  M5.Display.print("CONTROLS");
  M5.Display.setTextColor(DARKGREY);
  M5.Display.setCursor(150, 2);
  M5.Display.print("HOLD = 1 sec");

  M5.Display.drawFastHLine(0, 12, SCREEN_WIDTH, CYAN);

  M5.Display.setTextColor(YELLOW);
  M5.Display.setCursor(2, 17);
  M5.Display.print("SCANNING / LIST");

  M5.Display.setTextColor(WHITE);
  M5.Display.setCursor(2, 28);
  M5.Display.print(" A       Next page / next device");
  M5.Display.setCursor(2, 38);
  M5.Display.print(" B       Filter: All/Named/Alerts");
  M5.Display.setTextColor(GREEN);
  M5.Display.setCursor(2, 48);
  M5.Display.print(" HOLD A  Stop (detail view) / start");
  M5.Display.setCursor(2, 58);
  M5.Display.print(" HOLD B  Open settings menu");

  M5.Display.setTextColor(YELLOW);
  M5.Display.setCursor(2, 72);
  M5.Display.print("SETTINGS MENU");

  M5.Display.setTextColor(WHITE);
  M5.Display.setCursor(2, 83);
  M5.Display.print(" A       Next option");
  M5.Display.setCursor(2, 93);
  M5.Display.print(" B       Select");
  M5.Display.setTextColor(GREEN);
  M5.Display.setCursor(2, 103);
  M5.Display.print(" HOLD B  Close menu");

  M5.Display.drawFastHLine(0, 115, SCREEN_WIDTH, MAGENTA);
  M5.Display.setTextColor(DARKGREY);
  M5.Display.setCursor(2, 120);
  M5.Display.print("Alert: any button dismisses");
  M5.Display.setTextColor(YELLOW);
  M5.Display.setCursor(168, 120);
  M5.Display.print("A/B: OK");
}

// Draws the sheet and blocks until a button press, or until timeoutMs elapses
// (pass 0 to wait indefinitely). Core 1 only — it touches buttons and display.
void showControlsScreen(unsigned long timeoutMs) {
  drawControlsScreen();

  unsigned long start = millis();
  while (timeoutMs == 0 || millis() - start < timeoutMs) {
    M5.update();
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) break;
    delay(20);
  }

  // Leave the button released before returning, so the press that dismissed
  // this screen doesn't also get read as a gesture by whatever comes next.
  // Bounded, so a button wedged against something can't strand boot here.
  unsigned long releaseWait = millis();
  while ((M5.BtnA.isPressed() || M5.BtnB.isPressed()) &&
         millis() - releaseWait < 3000) {
    M5.update();
    delay(10);
  }
  M5.update();
}

void printControlsToSerial() {
  Serial.println("--- PathShield controls (HOLD = press for 1 second) ---");
  Serial.println("  Scanning / list:");
  Serial.println("    A         Next page of the list (next device when stopped)");
  Serial.println("    B         Cycle filter: All -> Named -> Alerts");
  Serial.println("    HOLD A    Stop / start scanning (stopped = per-device detail)");
  Serial.println("    HOLD B    Open the settings menu");
  Serial.println("  Settings menu:");
  Serial.println("    A         Next option");
  Serial.println("    B         Select");
  Serial.println("    HOLD B    Close the menu");
  Serial.println("  Alert screen: either button dismisses.");
}

void displayStartupMessage() {
  M5.Display.fillScreen(BLACK);

  for (int i = 0; i < 20; i++) {
    int x = random(0, SCREEN_WIDTH);
    int y = random(0, SCREEN_HEIGHT);
    int w = random(5, 40);
    M5.Display.drawFastHLine(x, y, w, random(0x0000, 0x1111));
  }

  M5.Display.drawFastHLine(0, 20, SCREEN_WIDTH, CYAN);
  M5.Display.drawFastHLine(0, 21, SCREEN_WIDTH, CYAN);

  M5.Display.setTextSize(3);
  M5.Display.setTextColor(CYAN);
  M5.Display.setCursor(15, 30);
  M5.Display.print("PATH");
  M5.Display.setTextColor(MAGENTA);
  M5.Display.print("SHIELD");

  M5.Display.setTextColor(0x07E0);
  M5.Display.setCursor(16, 31);
  M5.Display.print("PATH");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(YELLOW);
  M5.Display.setCursor(50, 60);
  M5.Display.print("TRACKER DETECTION");

  M5.Display.setTextColor(DARKGREY);
  M5.Display.setCursor(85, 72);
  M5.Display.print("v" FIRMWARE_VERSION);

  M5.Display.drawFastHLine(0, 85, SCREEN_WIDTH, MAGENTA);

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(GREEN);

  M5.Display.setCursor(10, 95);
  M5.Display.print("> Initializing BLE/WiFi");
  delay(400);

  M5.Display.setCursor(190, 95);
  M5.Display.setTextColor(CYAN);
  M5.Display.print("[OK]");
  delay(300);

  M5.Display.setTextColor(GREEN);
  M5.Display.setCursor(10, 107);
  M5.Display.print("> Loading SPIFFS");
  delay(400);

  M5.Display.setCursor(190, 107);
  M5.Display.setTextColor(CYAN);
  M5.Display.print("[OK]");
  delay(300);

  M5.Display.setTextColor(GREEN);
  M5.Display.setCursor(10, 119);
  M5.Display.print("> System Ready");
  delay(300);

  M5.Display.setCursor(190, 119);
  M5.Display.setTextColor(CYAN);
  M5.Display.print("[OK]");

  delay(400);
  M5.Display.fillScreen(CYAN);
  delay(50);
  M5.Display.fillScreen(BLACK);
  delay(50);
}

// Which list the findings screen should render. The scanner keeps alternating
// bands regardless — this only decides what's on screen. With a BLE-side
// filter active the view pins to the BLE list rather than flipping to the
// unfiltered WiFi list every few seconds, which read as the filter switching
// itself off and back on.
bool isWifiView() {
  return scanningWiFi && filterMode == FILTER_ALL;
}

// Two cases draw a single list merged across both bands, so they are neither
// the WiFi view nor the BLE view:
//
//   - the ALERTS filter, so a WiFi privacy-invader hit isn't hidden by a
//     filter named "Alerts";
//   - anything paused, because the band the view happens to be pinned to when
//     you stop scanning is arbitrary. isWifiView() keys off scanningWiFi, which
//     is frozen while paused, so pausing during a BLE window used to strand you
//     on BLE with no way to reach the WiFi list at all.
bool isMergedView() {
  return filterMode == FILTER_ALERTS || paused;
}

// Whether the merged view shows everything or only what's flagged. Pausing is
// for looking over what you caught, so it shows the lot; the ALERTS filter is
// for cutting to what matters, so it doesn't.
bool mergedViewAlertsOnly() {
  return filterMode == FILTER_ALERTS;
}

// Top-bar label. In the merged alerts view the screen isn't showing one band's
// list, so report the band actually being scanned rather than mislabelling the
// view as one or the other.
const char *scanModeLabel() {
  if (paused) return "PAUSE";
  if (isMergedView()) return scanningWiFi ? "WiFi" : "BLE";
  return displayingWifiView ? "WiFi" : "BLE";
}

const char *filterLabel(FilterMode mode) {
  switch (mode) {
    case FILTER_NAMED:  return "NAMED";
    case FILTER_ALERTS: return "ALERTS";
    default:            return "ALL";
  }
}

uint16_t filterColor(FilterMode mode) {
  switch (mode) {
    case FILTER_NAMED:  return CYAN;
    case FILTER_ALERTS: return RED;
    default:            return ORANGE;
  }
}

// Battery percentage, smoothed. Two things were wrong with reading it inline:
// the old (V - 3.0) / 1.2 lerp treats a lithium cell's discharge curve as a
// straight line, so it reads high for most of a session and then falls off a
// cliff; and an unfiltered sample redrawn every second visibly jitters.
// M5Unified's getBatteryLevel() already does the curve properly for this
// hardware, so this just guards it and applies an exponential moving average.
// Returns 0-100.
int batteryPercent() {
  static float smoothed = -1.0f;

  int32_t level = M5.Power.getBatteryLevel();

  // Negative means the driver couldn't read it; keep the last good value
  // rather than flashing a bogus 0% and tripping the low-battery path.
  if (level < 0 || level > 100) {
    return (smoothed < 0.0f) ? 100 : (int)(smoothed + 0.5f);
  }

  if (smoothed < 0.0f) {
    smoothed = (float)level;
  } else {
    smoothed += ((float)level - smoothed) * 0.2f;
  }
  return (int)(smoothed + 0.5f);
}

// The surface the repainting screens draw into. Both M5GFX and M5Canvas derive
// from LovyanGFX, so this is the sprite when it allocated and the panel itself
// when it didn't — same output either way, the fallback just flickers like it
// used to instead of failing to draw at all.
inline LovyanGFX &frame() {
  return canvasReady ? static_cast<LovyanGFX &>(frameCanvas)
                     : static_cast<LovyanGFX &>(M5.Display);
}

// Send a composed frame to the panel. No-op on the fallback path, where the
// drawing already went straight there.
inline void framePush() {
  if (canvasReady) frameCanvas.pushSprite(0, 0);
}

// Signal strength as a four-bar glyph, drawn from (x, y) in a 21x8 box so it
// occupies roughly the same width as the "-75dB" text it replaces. Thresholds
// are the usual BLE/WiFi rules of thumb: -60 and up is close, -70 nearby,
// -80 present-but-distant, below that is at the edge of RSSI_FLOOR.
void drawSignalBars(LovyanGFX &gfx, int x, int y, int rssi, uint16_t color) {
  int bars = 1;
  if (rssi >= -60)      bars = 4;
  else if (rssi >= -70) bars = 3;
  else if (rssi >= -80) bars = 2;

  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    int bx = x + i * 5;
    int by = y + (8 - h);
    if (i < bars) {
      gfx.fillRect(bx, by, 4, h, color);
    } else {
      // Unlit bars still drawn, so the glyph reads as "1 of 4" rather than as
      // a shape that changes size with the signal.
      gfx.drawRect(bx, by, 4, h, BLUE_GREY);
    }
  }
}

// Composes into the shared frame — the caller is responsible for framePush().
void drawTopBar() {
  auto &gfx = frame();

  gfx.drawFastHLine(0, 0, SCREEN_WIDTH, CYAN);
  gfx.drawFastHLine(0, 1, SCREEN_WIDTH, CYAN);

  gfx.setTextSize(1);
  gfx.setCursor(4, 3);
  gfx.setTextColor(paused ? RED : GREEN);
  gfx.print(scanModeLabel());

  // Reading only — the critical-battery shutdown lives in loop() now. It had no
  // business running inside a render function, where it blocked on a 3-second
  // delay while holding deviceMutex.
  int batPercent = batteryPercent();

  int barWidth = 30;
  int barX = 75;
  int barY = 4;

  uint16_t barColor = GREEN;
  if (batPercent < 50) barColor = YELLOW;
  if (batPercent < 25) barColor = RED;

  gfx.setTextColor(BLUE_GREY);
  gfx.setCursor(45, 3);
  gfx.print("BATT:");

  gfx.drawRect(barX, barY, barWidth, 6, BLUE_GREY);
  int filledWidth = (barWidth - 2) * batPercent / 100;
  gfx.fillRect(barX + 1, barY + 1, filledWidth, 4, barColor);

  gfx.setTextColor(DARKGREY);
  gfx.setCursor(110, 3);
  gfx.print(batPercent);
  gfx.print("%");

  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t freeKB = freeHeap / 1024;

  // Scale memory display dynamically: initialFreeHeapKB = 100%, 0 = 0%
  int memPercent = (initialFreeHeapKB > 0) ? (freeKB * 100) / initialFreeHeapKB : 0;
  if (memPercent > 100) memPercent = 100;
  if (memPercent < 0) memPercent = 0;

  barX = 155;
  uint16_t memColor = GREEN;
  if (freeKB < MEMORY_WARNING) memColor = YELLOW;
  if (freeKB < MEMORY_CRITICAL) memColor = RED;

  gfx.setCursor(135, 3);
  gfx.setTextColor(BLUE_GREY);
  gfx.print("MEM:");

  gfx.drawRect(barX, barY, barWidth, 6, BLUE_GREY);
  filledWidth = (barWidth - 2) * memPercent / 100;
  gfx.fillRect(barX + 1, barY + 1, filledWidth, 4, memColor);

  gfx.setTextColor(memColor);
  gfx.setCursor(190, 3);
  gfx.print(freeKB);
  gfx.print("KB");

  gfx.drawFastHLine(0, 11, SCREEN_WIDTH, CYAN);
  gfx.drawFastHLine(0, 12, SCREEN_WIDTH, CYAN);
}

// Generate hashed display state
uint32_t getDisplayStateHash() {
  uint32_t hash = 1;

  hash = hash * 31 + deviceIndex;
  hash = hash * 31 + wifiDeviceIndex;
  hash = hash * 31 + scrollIndex;
  hash = hash * 31 + (isWifiView() ? 1 : 0);
  hash = hash * 31 + (scanningWiFi ? 1 : 0);
  hash = hash * 31 + (paused ? 1 : 0);
  hash = hash * 31 + (uint32_t)filterMode;
  hash = hash * 31 + (uint32_t)unackedAlertCount;

  // The merged alerts view renders rows from both arrays, so it has to watch
  // both — hashing only one would leave the other's changes off screen.
  if (isWifiView() || isMergedView()) {
    for (int i = 0; i < wifiDeviceIndex; i++) {
      hash = hash * 31 + (uint32_t)wifiDevices[i].rssi;
      hash = hash * 31 + wifiDevices[i].detectionCount;
      hash = hash * 31 + wifiDevices[i].channel;
      hash = hash * 31 + wifiDevices[i].encryptionType;
      hash = hash * 31 + (wifiDevices[i].isSpecial ? 1 : 0);
      for (int j = 0; j < strlen(wifiDevices[i].ssid); j++) {
        hash = hash * 31 + wifiDevices[i].ssid[j];
      }
    }
  }

  if (!isWifiView()) {
    for (int i = 0; i < deviceIndex; i++) {
      hash = hash * 31 + trackedDevices[i].totalCount;
      hash = hash * 31 + (uint32_t)trackedDevices[i].lastRssi;
      hash = hash * 31 + (trackedDevices[i].detected ? 1 : 0);
      hash = hash * 31 + (trackedDevices[i].isSpecial ? 1 : 0);
      for (int j = 0; j < strlen(trackedDevices[i].name); j++) {
        hash = hash * 31 + trackedDevices[i].name[j];
      }
      for (int j = 0; j < strlen(trackedDevices[i].manufacturer); j++) {
        hash = hash * 31 + trackedDevices[i].manufacturer[j];
      }
    }
  }

  return hash;
}

// Bottom strip of the findings screen: which filter is live, plus the two
// gestures that aren't discoverable by mashing buttons. Tap actions are on the
// controls screen (boot + settings menu) — there isn't room for all six here.
// Composes into the shared frame — the caller is responsible for framePush().
void drawListFooterHint() {
  auto &gfx = frame();

  gfx.drawFastHLine(0, 133, SCREEN_WIDTH, CYAN);
  gfx.drawFastHLine(0, 134, SCREEN_WIDTH, CYAN);

  gfx.setTextSize(1);
  gfx.setTextColor(filterColor(filterMode));
  gfx.setCursor(2, 124);
  const char *tag = filterLabel(filterMode);
  gfx.print(tag);

  gfx.setTextColor(GREEN);
  const char *hints = paused ? "HOLD A:Scan B:Menu" : "HOLD A:Stop B:Menu";
  gfx.setCursor(2 + strlen(tag) * 6 + 8, 124);
  gfx.print(hints);

  // An alert that timed out unacknowledged shouldn't disappear without trace.
  // The whole screen edge goes red rather than a banner row taking a fourth of
  // a 135px-tall screen: at this size the footer is already carrying the filter
  // tag, both hold hints and the page counter, and the border was otherwise
  // decorative. The count sits between the hints and the page counter, which is
  // the one gap wide enough for it at every filter/page combination.
  if (unackedAlertCount > 0) {
    gfx.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, RED);
    gfx.drawRect(1, 1, SCREEN_WIDTH - 2, SCREEN_HEIGHT - 2, RED);

    char badge[6];
    snprintf(badge, sizeof(badge), "!%d", unackedAlertCount);
    gfx.setTextColor(RED);
    gfx.setCursor(2 + strlen(tag) * 6 + 8 + strlen(hints) * 6 + 6, 124);
    gfx.print(badge);
  }
}

// Takes an index rather than a DeviceInfo& deliberately: Arduino hoists its
// generated prototypes above the struct definitions, so naming those types in a
// signature breaks the build. RowSnapshot is declared at the top of the file for
// the same reason.
static void snapshotBleRow(RowSnapshot &r, int idx) {
  const DeviceInfo &d = trackedDevices[idx];
  r.isWifi = false;
  r.detected = d.detected;
  r.isSpecial = d.isSpecial;
  strncpy(r.name, d.name, 32);
  r.name[32] = '\0';
  strncpy(r.address, d.address, 17);
  r.address[17] = '\0';
  strncpy(r.manufacturer, d.manufacturer, 30);
  r.manufacturer[30] = '\0';
  r.trackerType = d.trackerType;
  r.score = d.persistenceScore;
  r.firstSeen = d.firstSeen;
  r.totalCount = d.totalCount;
  r.rssi = d.lastRssi;
  r.minRssi = d.minRssi;
  r.maxRssi = d.maxRssi;
  r.avgRssi = (d.rssiCount > 0) ? (d.rssiSum / d.rssiCount) : d.lastRssi;
  r.channel = 0;
  r.encryptionType = 0;
}

static void snapshotWifiRow(RowSnapshot &r, int idx) {
  const WiFiDeviceInfo &w = wifiDevices[idx];
  r.isWifi = true;
  // A WiFi row is "detected" exactly when it's a privacy-invader OUI match —
  // there's no persistence scoring on that band.
  r.detected = w.isSpecial;
  r.isSpecial = w.isSpecial;
  strncpy(r.name, w.ssid, 32);
  r.name[32] = '\0';
  strncpy(r.address, w.bssid, 17);
  r.address[17] = '\0';
  getManufacturer(w.bssid, r.manufacturer, 31);
  r.trackerType = w.isSpecial ? TRACKER_PRIVACY_INVADER : TRACKER_NONE;
  r.score = 0.0f;
  r.firstSeen = 0;
  r.totalCount = w.detectionCount;
  r.rssi = w.rssi;
  r.minRssi = w.rssi;
  r.maxRssi = w.rssi;
  r.avgRssi = w.rssi;
  r.channel = w.channel;
  r.encryptionType = w.encryptionType;
}

// Stopping the scan switches from a three-row list to one device at a time,
// with everything known about it. Every button is already assigned in both
// modes, so there was no gesture left to open a detail view with — but stopping
// already means "I want to look at this properly", and a frozen three-row list
// is a weaker use of the screen than a full record of one device. Tap A still
// steps through, so scrolling is unchanged; there's just one device per step.
bool isDetailView() {
  return paused;
}

// Collects the rows this frame will draw — three in list mode, one in the
// paused detail view. MUST be called with deviceMutex held; touches nothing
// but the device arrays and the snapshot.
void buildFrameSnapshot(FrameSnapshot &s) {
  const int maxDisplay = isDetailView() ? 1 : 3;

  s.rowsFilled = 0;
  s.totalItems = 0;
  s.wifiView = isWifiView();
  s.merged = isMergedView();
  s.alertsOnly = mergedViewAlertsOnly();
  s.bleTracked = deviceIndex;
  s.wifiTracked = wifiDeviceIndex;
  s.nothingTracked = (deviceIndex == 0 && wifiDeviceIndex == 0);

  if (s.nothingTracked) {
    s.scrollIndex = 0;
    topVisibleValid = false;
    return;
  }

  if (s.merged) {
    // One list across both bands. "Alerts" that silently omitted every WiFi hit
    // would be a trap now that a privacy-invader OUI can match a BSSID, and a
    // paused browse needs to reach both lists since neither is being scanned.
    // Definite hits (an OUI match on either band) sort above persistence-scored
    // suspicions, then by score — certainty first, then confidence.
    struct MergedRef { bool isWifi; int idx; float rank; };
    MergedRef refs[MAX_DEVICES_CAP + MAX_WIFI_DEVICES_CAP];
    int count = 0;

    for (int i = 0; i < deviceIndex; i++) {
      if (s.alertsOnly && !trackedDevices[i].detected) continue;
      // The merged view still honours NAMED — pausing shouldn't quietly undo
      // a filter the user set.
      if (filterMode == FILTER_NAMED && strlen(trackedDevices[i].name) == 0) continue;
      refs[count].isWifi = false;
      refs[count].idx = i;
      // Flagged devices outrank background ones, so a paused browse still puts
      // anything alerting at the top rather than burying it mid-list.
      refs[count].rank = trackedDevices[i].detected
                             ? (trackedDevices[i].isSpecial ? 2.0f : 1.0f) +
                                   trackedDevices[i].persistenceScore
                             : -1.0f;
      count++;
    }
    for (int i = 0; i < wifiDeviceIndex; i++) {
      if (s.alertsOnly && !wifiDevices[i].isSpecial) continue;
      if (filterMode == FILTER_NAMED && strlen(wifiDevices[i].ssid) == 0) continue;
      refs[count].isWifi = true;
      refs[count].idx = i;
      refs[count].rank = wifiDevices[i].isSpecial ? 3.0f : -2.0f;
      count++;
    }

    s.totalItems = count;
    if (count == 0) {
      s.scrollIndex = 0;
      topVisibleValid = false;
      return;
    }

    if (scrollIndex >= count) scrollIndex = 0;
    s.scrollIndex = scrollIndex;

    // Only the visible window needs ordering, so sort just that far. A full
    // sort of every row, every frame, was wasted work inside the lock.
    int needed = std::min(count, scrollIndex + maxDisplay);
    std::partial_sort(refs, refs + needed, refs + count,
                      [](const MergedRef &a, const MergedRef &b) {
                        return a.rank > b.rank;
                      });

    // The allowlist menu action is BLE-only, so it only arms when the top row
    // is a BLE device — otherwise it would appear to target the WiFi row on
    // screen and then act on something else entirely.
    if (!refs[scrollIndex].isWifi) {
      strncpy(topVisibleAddress, trackedDevices[refs[scrollIndex].idx].address, 17);
      topVisibleAddress[17] = '\0';
      topVisibleValid = true;
    } else {
      topVisibleValid = false;
    }

    for (int r = scrollIndex; r < count && s.rowsFilled < maxDisplay; r++) {
      if (refs[r].isWifi) {
        snapshotWifiRow(s.rows[s.rowsFilled], refs[r].idx);
      } else {
        snapshotBleRow(s.rows[s.rowsFilled], refs[r].idx);
      }
      s.rowsFilled++;
    }
    return;
  }

  if (s.wifiView) {
    // topVisibleAddress deliberately survives a WiFi frame: with no filter set
    // the view alternates bands every few seconds, and clearing it here would
    // make the menu's allowlist action available only half the time. The menu
    // shows the MAC tail it would act on, so it's never a blind press.
    s.totalItems = wifiDeviceIndex;
    // APs ageing out can strand scrollIndex past the end of the shorter list.
    if (scrollIndex >= wifiDeviceIndex) scrollIndex = 0;
    s.scrollIndex = scrollIndex;

    for (int i = scrollIndex; i < wifiDeviceIndex && s.rowsFilled < maxDisplay; i++) {
      snapshotWifiRow(s.rows[s.rowsFilled], i);
      s.rowsFilled++;
    }
    return;
  }

  // BLE list. FILTER_ALERTS never reaches here — it has its own merged branch.
  int refs[MAX_DEVICES_CAP];
  int count = 0;
  for (int i = 0; i < deviceIndex; i++) {
    if (filterMode == FILTER_NAMED && strlen(trackedDevices[i].name) == 0) continue;
    refs[count++] = i;
  }

  s.totalItems = count;
  if (count == 0) {
    s.scrollIndex = 0;
    topVisibleValid = false;
    return;
  }

  // Devices ageing out from under a filter can strand scrollIndex past the
  // end of the (now shorter) list.
  if (scrollIndex >= count) scrollIndex = 0;
  s.scrollIndex = scrollIndex;

  int needed = std::min(count, scrollIndex + maxDisplay);
  std::partial_sort(refs, refs + needed, refs + count, [](int a, int b) {
    const DeviceInfo &x = trackedDevices[a];
    const DeviceInfo &y = trackedDevices[b];
    if (x.detected != y.detected) return x.detected;
    if (x.detected) return x.persistenceScore > y.persistenceScore;
    return x.totalCount > y.totalCount;
  });

  strncpy(topVisibleAddress, trackedDevices[refs[scrollIndex]].address, 17);
  topVisibleAddress[17] = '\0';
  topVisibleValid = true;

  for (int i = scrollIndex; i < count && s.rowsFilled < maxDisplay; i++) {
    snapshotBleRow(s.rows[s.rowsFilled], refs[i]);
    s.rowsFilled++;
  }
}

// Draws one row of the findings list at y, returning the y for the next row.
// Reads only the snapshot — safe to call with no lock held.
static int drawListRow(LovyanGFX &gfx, const RowSnapshot &r, int y,
                       bool showBandTag, unsigned long nowSec) {
  uint16_t nameColor;
  if (r.isSpecial) {
    nameColor = ORANGE;
  } else if (r.isWifi) {
    nameColor = WIFI_NAME_COLOR;
  } else {
    nameColor = r.detected ? RED : BLE_NAME_COLOR;
  }

  gfx.setTextColor(nameColor);
  gfx.setTextSize(2);
  gfx.setCursor(2, y);

  char display[20];
  if (strlen(r.name) > 0) {
    strncpy(display, r.name, 16);
    display[16] = '\0';
    if (strlen(r.name) > 16) strcat(display, "...");
  } else {
    strcpy(display, r.isWifi ? "Hidden" : "Unknown");
  }
  gfx.print(display);
  y += 17;

  gfx.setTextSize(1);
  gfx.setCursor(2, y);

  int mfgBudget = 27;
  if (showBandTag) {
    gfx.setTextColor(nameColor);
    gfx.print(r.isWifi ? "[WiFi] " : "[BLE] ");
    mfgBudget -= 7;
  }
  if (!r.isWifi && r.trackerType != TRACKER_NONE) {
    gfx.setTextColor(RED);
    gfx.print("[");
    gfx.print(trackerTypeName(r.trackerType));
    gfx.print("] ");
    mfgBudget -= (int)strlen(trackerTypeName(r.trackerType)) + 3;
  }
  gfx.setTextColor(YELLOW);
  if (mfgBudget > 0) {
    char mfg[31];
    strncpy(mfg, r.manufacturer, std::min(mfgBudget, 30));
    mfg[std::min(mfgBudget, 30)] = '\0';
    gfx.print(mfg);
  }
  y += 9;

  gfx.setCursor(2, y);
  if (!r.isWifi && r.detected) {
    gfx.setTextColor(RED);
    gfx.print("!");
    gfx.setTextColor(YELLOW);
    gfx.print(r.score, 2);
    char durStr[10];
    unsigned long elapsed = (nowSec >= r.firstSeen) ? (nowSec - r.firstSeen) : 0;
    formatDuration(elapsed, durStr, sizeof(durStr));
    gfx.setTextColor(WHITE);
    gfx.print(" ");
    gfx.print(durStr);
  } else if (r.isWifi) {
    gfx.setTextColor(WHITE);
    gfx.printf("Ch%d ", r.channel);
    // The band-tagged merged view spends this line's remaining width on the
    // BSSID instead: on a row that's alerting, which device it is matters more
    // than how the AP is encrypted.
    if (!showBandTag) {
      gfx.setTextColor(GREEN);
      gfx.print(wifiAuthName(r.encryptionType));
      gfx.print(" ");
    }
    gfx.setTextColor(DARKGREY);
    gfx.print(r.totalCount);
    gfx.print("x ");
    if (showSignalBars) {
      drawSignalBars(gfx, gfx.getCursorX(), y, r.rssi, DARKGREY);
    } else {
      gfx.print(r.rssi);
      gfx.print("dB");
    }
  } else {
    gfx.setTextColor(WHITE);
    gfx.print(r.totalCount);
    gfx.print("x ");
    if (showSignalBars) {
      drawSignalBars(gfx, gfx.getCursorX(), y, r.rssi, WHITE);
    } else {
      gfx.print(r.rssi);
      gfx.print("dB");
    }
  }

  // The un-merged WiFi list spends this space on the encryption type instead,
  // which is the one view where the BSSID isn't the point.
  if (!r.isWifi || showBandTag) {
    gfx.setCursor(80, y);
    gfx.setTextColor(BLUE_GREY);
    gfx.print(r.address);
  }

  return y + 11;
}

// One device, everything known about it. Shown while stopped — see
// isDetailView(). Reads only the snapshot, so no lock is held.
static void drawDetailView(LovyanGFX &gfx, const RowSnapshot &r,
                           unsigned long nowSec) {
  int y = 16;

  uint16_t headColor = r.isSpecial ? ORANGE
                       : r.detected ? RED
                                    : (r.isWifi ? WIFI_NAME_COLOR : BLE_NAME_COLOR);

  gfx.setTextSize(2);
  gfx.setTextColor(headColor);
  gfx.setCursor(2, y);
  char nameDisplay[21];
  if (strlen(r.name) > 0) {
    strncpy(nameDisplay, r.name, 17);
    nameDisplay[17] = '\0';
    if (strlen(r.name) > 17) strcat(nameDisplay, "...");
  } else {
    strcpy(nameDisplay, r.isWifi ? "Hidden SSID" : "Unnamed");
  }
  gfx.print(nameDisplay);
  y += 19;

  gfx.setTextSize(1);

  // Full address, untruncated — this is the view you read a MAC off to write
  // it down, so it gets its own line at full width.
  gfx.setTextColor(WHITE);
  gfx.setCursor(2, y);
  gfx.print(r.isWifi ? "BSSID " : "MAC   ");
  gfx.setTextColor(BLUE_GREY);
  gfx.print(r.address);
  y += 10;

  gfx.setTextColor(WHITE);
  gfx.setCursor(2, y);
  gfx.print("Vendor ");
  gfx.setTextColor(YELLOW);
  char mfg[28];
  strncpy(mfg, r.manufacturer, 27);
  mfg[27] = '\0';
  gfx.print(mfg);
  y += 10;

  gfx.setTextColor(WHITE);
  gfx.setCursor(2, y);
  if (r.isWifi) {
    gfx.printf("WiFi   Ch%d  ", r.channel);
    gfx.setTextColor(GREEN);
    gfx.print(wifiAuthName(r.encryptionType));
  } else if (r.trackerType != TRACKER_NONE) {
    gfx.print("Type   ");
    gfx.setTextColor(RED);
    gfx.print(trackerTypeName(r.trackerType));
  } else {
    gfx.print("Type   ");
    gfx.setTextColor(DARKGREY);
    gfx.print("Unclassified BLE");
  }
  y += 10;

  // Signal as range, not a single reading: a device holding steady while you
  // move is the interesting case, and one number can't show that.
  gfx.setTextColor(WHITE);
  gfx.setCursor(2, y);
  gfx.print("Signal ");
  if (showSignalBars) {
    drawSignalBars(gfx, gfx.getCursorX(), y, r.rssi, WHITE);
    gfx.setCursor(gfx.getCursorX() + 24, y);
  }
  gfx.setTextColor(DARKGREY);
  gfx.printf("%d now", r.rssi);
  if (!r.isWifi && r.minRssi != r.maxRssi) {
    gfx.printf(" (%d..%d avg %d)", r.minRssi, r.maxRssi, r.avgRssi);
  }
  y += 10;

  gfx.setTextColor(WHITE);
  gfx.setCursor(2, y);
  gfx.printf("Seen   %dx", r.totalCount);
  if (!r.isWifi) {
    char durStr[10];
    unsigned long elapsed = (nowSec >= r.firstSeen) ? (nowSec - r.firstSeen) : 0;
    formatDuration(elapsed, durStr, sizeof(durStr));
    gfx.printf(" over %s", durStr);
  }
  y += 10;

  gfx.setTextColor(WHITE);
  gfx.setCursor(2, y);
  gfx.print("Score  ");
  if (r.isWifi) {
    gfx.setTextColor(r.isSpecial ? ORANGE : DARKGREY);
    gfx.print(r.isSpecial ? "OUI match (immediate)" : "not scored on WiFi");
  } else {
    gfx.setTextColor(r.detected ? RED : DARKGREY);
    gfx.printf("%.2f", r.score);
    gfx.setTextColor(DARKGREY);
    gfx.printf(" / %.2f to alert", persistenceThreshold);
  }
}

// Renders a prepared snapshot. No lock held — see displayTrackedDevices().
void renderFrameSnapshot(const FrameSnapshot &s, unsigned long nowSec) {
  auto &gfx = frame();

  gfx.fillScreen(BLACK);
  drawTopBar();

  if (s.nothingTracked) {
    lastVisibleItemCount = 0;
    gfx.setTextSize(2);
    gfx.setTextColor(DARKGREY);
    gfx.setCursor(55, 60);
    gfx.print(paused ? "Stopped" : "Scanning...");
    drawListFooterHint();
    framePush();
    return;
  }

  // A filter that matches nothing yet should say so, not leave a blank screen
  // that looks like the device stopped working.
  if (s.totalItems == 0) {
    lastVisibleItemCount = 0;
    gfx.setTextSize(2);
    gfx.setTextColor(filterColor(filterMode));
    gfx.setCursor(2, 45);
    gfx.print(s.alertsOnly                  ? "No alerts"
              : filterMode == FILTER_NAMED  ? "No named"
                                            : "Nothing yet");
    gfx.setTextSize(1);
    gfx.setTextColor(DARKGREY);
    gfx.setCursor(2, 70);
    if (s.alertsOnly) {
      gfx.printf("%d BLE / %d WiFi tracked, none flagged", s.bleTracked,
                 s.wifiTracked);
    } else {
      gfx.printf("%d BLE / %d WiFi tracked, none match", s.bleTracked,
                 s.wifiTracked);
    }
    drawListFooterHint();
    framePush();
    return;
  }

  if (isDetailView() && s.rowsFilled > 0) {
    drawDetailView(gfx, s.rows[0], nowSec);
  } else {
    int y = 15;
    for (int i = 0; i < s.rowsFilled; i++) {
      if (i > 0) {
        gfx.drawFastHLine(0, y - 2, SCREEN_WIDTH, BLUE_GREY);
        y += 1;
      }
      y = drawListRow(gfx, s.rows[i], y, s.merged, nowSec);
    }
  }

  lastVisibleItemCount = s.totalItems;

  drawListFooterHint();

  gfx.setTextSize(1);
  gfx.setTextColor(YELLOW);
  char countStr[20];
  snprintf(countStr, sizeof(countStr), "%d-%d/%d", s.scrollIndex + 1,
           s.scrollIndex + s.rowsFilled, s.totalItems);
  int xPos = SCREEN_WIDTH - (int)strlen(countStr) * 6 - 4;
  gfx.setCursor(xPos, 124);
  gfx.print(countStr);

  framePush();
}

void displayTrackedDevices() {
  unsigned long now = millis();

  if (now - lastDisplayRender < 500) {
    return;
  }

  if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }

  uint32_t currentHash = getDisplayStateHash();
  if (currentHash == lastStateHash) {
    xSemaphoreGive(deviceMutex);
    return;
  }

  lastStateHash = currentHash;
  lastDisplayRender = now;
  displayingWifiView = isWifiView();

  // Copy out what this frame needs, then get off the lock before drawing any
  // of it. The draw used to run start to finish with deviceMutex held —
  // including every SPI write to the panel — while scanTask on Core 0 needs
  // that same mutex to record what it just scanned and gives up after 2000ms.
  // A slow frame could therefore make an entire scan batch get discarded.
  static FrameSnapshot snapshot;
  buildFrameSnapshot(snapshot);

  xSemaphoreGive(deviceMutex);

  renderFrameSnapshot(snapshot, now / 1000);
}

void displayMenuScreen() {
  unsigned long now = millis();

  if (now - lastMenuRender < 500) {
    return;
  }
  lastMenuRender = now;

  auto &gfx = frame();

  gfx.fillScreen(BLACK);
  gfx.setCursor(2, 2);
  gfx.setTextColor(GREEN);
  gfx.setTextSize(1);
  gfx.print("SETTINGS");

  // The splash flashes past in a couple of seconds; this is the version you
  // can go and look up at any time to confirm what's actually flashed.
  gfx.setTextColor(DARKGREY);
  gfx.setCursor(SCREEN_WIDTH - 4 - (int)(strlen(FIRMWARE_VERSION) + 1) * 6, 2);
  gfx.print("v" FIRMWARE_VERSION);

  gfx.drawLine(0, 12, SCREEN_WIDTH, 12, DARKGREY);

  int y = 15;

  gfx.setTextColor(WHITE);
  gfx.setCursor(2, y);

  int batPercent = batteryPercent();
  float estHoursRemaining = (batPercent / 100.0f) * TYPICAL_BATTERY_LIFE_HOURS;
  // Everything status-y on one line — ten menu rows need the vertical space.
  gfx.print("Bat:");
  gfx.print(batPercent);
  gfx.print("% ~");
  if (estHoursRemaining >= 1.0f) {
    gfx.print(estHoursRemaining, 1);
    gfx.print("h");
  } else {
    gfx.print((int)(estHoursRemaining * 60));
    gfx.print("m");
  }
  gfx.print(" RAM:");
  gfx.print(ESP.getFreeHeap() / 1024);
  gfx.print("K Trk:");
  gfx.print(deviceIndex);
  y += 11;

  gfx.drawLine(0, y, SCREEN_WIDTH, y, DARKGREY);
  y += 3;

  menuBaseY = y;

  char timeoutStr[12];
  snprintf(timeoutStr, sizeof(timeoutStr), "%lus", screenTimeoutMs / 1000);

  // Show the MAC tail the allowlist action would actually consume, so it's a
  // confirmable choice rather than a blind press.
  char allowTarget[9] = "--";
  if (topVisibleValid && strlen(topVisibleAddress) >= 8) {
    strncpy(allowTarget, topVisibleAddress + strlen(topVisibleAddress) - 8, 8);
    allowTarget[8] = '\0';
  }

  // Label + current value per row, so the menu reads as state rather than as
  // a list of verbs whose effect you only learn by pressing them.
  const char *labels[MENU_OPTION_COUNT] = {
    "Alert Style",  "Alert Sound",     "Brightness",    "Screen Timeout",
    "Signal Display", "Allowlist Top Device", "Export Incident",
    "Clear Devices", "Show Controls", "Shutdown"
  };
  const char *values[MENU_OPTION_COUNT] = {
    alertStyleName(alertStyle),
    alertSoundEnabled ? "ON" : "OFF",
    highBrightness ? "HIGH" : "LOW",
    timeoutStr,
    showSignalBars ? "BARS" : "dBm",
    allowTarget,
    "", "", "", ""
  };

  // Inverse video for the selected row rather than a ">" in the margin — at
  // this size a single caret is easy to lose, and the highlight reads as
  // "you are here" from across a room.
  for (int i = 0; i < MENU_OPTION_COUNT; i++) {
    bool selected = (i == menuIndex);

    if (selected) {
      gfx.fillRect(0, y - 1, SCREEN_WIDTH, MENU_ROW_H, CYAN);
    }

    gfx.setTextColor(selected ? BLACK : CYAN);
    gfx.setCursor(4, y);
    gfx.print(labels[i]);
    if (strlen(values[i]) > 0) {
      gfx.setTextColor(selected ? BLACK : WHITE);
      gfx.setCursor(SCREEN_WIDTH - 4 - (int)(strlen(values[i]) * 6), y);
      gfx.print(values[i]);
    }
    y += MENU_ROW_H;
  }

  gfx.drawLine(0, y, SCREEN_WIDTH, y, DARKGREY);

  gfx.setTextColor(YELLOW);
  gfx.setTextSize(1);
  gfx.setCursor(2, y + 3);
  gfx.print("A:Next  B:Select  HOLD B:Close");

  framePush();
}

// Buffered rendering has no cheap partial update to offer: the frame is rebuilt
// and pushed whole either way, which is still fewer SPI transfers than the old
// per-field repaint this replaces. So moving the selection just forces that
// rebuild immediately instead of waiting out displayMenuScreen()'s throttle,
// which is what keeps menu nav feeling instant.
void highlightMenuOption(int index) {
  menuIndex = index;
  lastMenuRender = 0;
  displayMenuScreen();
}

void saveUserPreferences() {
  File file = SPIFFS.open("/prefs.txt", FILE_WRITE);
  if (!file) {
    return;
  }

  file.print("brightness=");
  file.println(highBrightness ? "1" : "0");
  file.print("timeout=");
  file.println(screenTimeoutMs);
  file.print("alertstyle=");
  file.println((int)alertStyle);
  file.print("sound=");
  file.println(alertSoundEnabled ? "1" : "0");
  file.print("signalbars=");
  file.println(showSignalBars ? "1" : "0");
  file.print("persistThresh=");
  file.println(persistenceThreshold, 2);
  file.print("rssiStable=");
  file.println(rssiStabilityThreshold);
  file.print("rssiVariation=");
  file.println(rssiVariationThreshold);

  file.close();
}

void loadUserPreferences() {
  File file = SPIFFS.open("/prefs.txt", FILE_READ);
  if (!file) {
    return; // File doesn't exist, use defaults
  }

  bool sawAlertStyle = false;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();

    if (line.startsWith("brightness=")) {
      highBrightness = line.substring(11).toInt() == 1;
    } else if (line.startsWith("timeout=")) {
      screenTimeoutMs = line.substring(8).toInt();
    } else if (line.startsWith("alertstyle=")) {
      int v = line.substring(11).toInt();
      if (v >= ALERT_LOUD && v <= ALERT_QUIET) alertStyle = (AlertStyle)v;
      sawAlertStyle = true;
    } else if (line.startsWith("discreet=")) {
      // Pre-alertstyle prefs file: the old boolean only distinguished quiet
      // from the strobe, so map it onto the closest of the three styles.
      if (!sawAlertStyle) {
        alertStyle = line.substring(9).toInt() == 1 ? ALERT_QUIET : ALERT_LOUD;
      }
    } else if (line.startsWith("sound=")) {
      alertSoundEnabled = line.substring(6).toInt() == 1;
    } else if (line.startsWith("signalbars=")) {
      showSignalBars = line.substring(11).toInt() == 1;
    } else if (line.startsWith("persistThresh=")) {
      float v = line.substring(14).toFloat();
      if (v >= 0.0f && v <= 1.0f) persistenceThreshold = v;
    } else if (line.startsWith("rssiStable=")) {
      int v = line.substring(11).toInt();
      if (v >= 1 && v <= 50) rssiStabilityThreshold = v;
    } else if (line.startsWith("rssiVariation=")) {
      int v = line.substring(14).toInt();
      if (v >= 1 && v <= 50) rssiVariationThreshold = v;
    }
  }

  file.close();
}

void saveRuntimeAllowlist() {
  File file = SPIFFS.open("/allowlist.txt", FILE_WRITE);
  if (!file) {
    return;
  }
  for (int i = 0; i < runtimeAllowlistCount; i++) {
    file.println(runtimeAllowlist[i]);
  }
  file.close();
}

void loadRuntimeAllowlist() {
  File file = SPIFFS.open("/allowlist.txt", FILE_READ);
  if (!file) {
    return; // File doesn't exist, nothing allowlisted yet
  }

  while (file.available() && runtimeAllowlistCount < MAX_RUNTIME_ALLOWLIST) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      strncpy(runtimeAllowlist[runtimeAllowlistCount], line.c_str(), 17);
      runtimeAllowlist[runtimeAllowlistCount][17] = '\0';
      runtimeAllowlistCount++;
    }
  }

  file.close();
}

// Removes a device from the runtime allowlist by exact MAC match — the inverse
// of allowlistDevice(). No in-field button gesture for this (by the time a
// device is allowlisted it's no longer on the tracked list to point a
// long-press at), so it's serial-command only via "allow remove".
bool removeFromAllowlist(const char *address) {
  if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    return false;
  }

  bool found = false;
  for (int i = 0; i < runtimeAllowlistCount; i++) {
    // Case-insensitive to match isAllowlistedMac(). The menu action stores the
    // lowercase address BLE reports, while the README documents `allow remove`
    // with an uppercase MAC — comparing exactly would leave an entry that can
    // be matched against but never removed.
    if (strcasecmp(runtimeAllowlist[i], address) == 0) {
      for (int j = i; j < runtimeAllowlistCount - 1; j++) {
        strcpy(runtimeAllowlist[j], runtimeAllowlist[j + 1]);
      }
      runtimeAllowlistCount--;
      found = true;
      break;
    }
  }

  xSemaphoreGive(deviceMutex);
  if (found) saveRuntimeAllowlist();
  return found;
}

void resetSpecialMacsToDefault() {
  specialMacsCount = 0;
  for (size_t i = 0; i < sizeof(defaultSpecialMacs) / sizeof(defaultSpecialMacs[0])
                      && specialMacsCount < MAX_SPECIAL_MACS; i++) {
    strncpy(specialMacs[specialMacsCount], defaultSpecialMacs[i], 17);
    specialMacs[specialMacsCount][17] = '\0';
    specialMacsCount++;
  }
}

void saveSpecialMacs() {
  File file = SPIFFS.open("/specialmacs.txt", FILE_WRITE);
  if (!file) {
    return;
  }
  for (int i = 0; i < specialMacsCount; i++) {
    file.println(specialMacs[i]);
  }
  file.close();
}

void loadSpecialMacs() {
  File file = SPIFFS.open("/specialmacs.txt", FILE_READ);
  if (!file) {
    resetSpecialMacsToDefault(); // No saved list yet — use compiled-in defaults
    return;
  }

  specialMacsCount = 0;
  while (file.available() && specialMacsCount < MAX_SPECIAL_MACS) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      strncpy(specialMacs[specialMacsCount], line.c_str(), 17);
      specialMacs[specialMacsCount][17] = '\0';
      specialMacsCount++;
    }
  }

  file.close();
}

bool addSpecialMac(const char *prefix) {
  if (specialMacsCount >= MAX_SPECIAL_MACS) {
    return false;
  }

  if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    return false;
  }

  strncpy(specialMacs[specialMacsCount], prefix, 17);
  specialMacs[specialMacsCount][17] = '\0';
  specialMacsCount++;

  xSemaphoreGive(deviceMutex);
  saveSpecialMacs();
  return true;
}

bool removeSpecialMac(int index) {
  if (index < 0 || index >= specialMacsCount) {
    return false;
  }

  if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    return false;
  }

  for (int i = index; i < specialMacsCount - 1; i++) {
    strcpy(specialMacs[i], specialMacs[i + 1]);
  }
  specialMacsCount--;

  xSemaphoreGive(deviceMutex);
  saveSpecialMacs();
  return true;
}

// Allowlists a device in the field and makes it disappear from the tracked
// list immediately — not just "won't alert again", but gone from view now,
// same as if it had never been seen. isAllowlistedMac() then keeps it out
// going forward.
bool allowlistDevice(const char *address) {
  if (runtimeAllowlistCount >= MAX_RUNTIME_ALLOWLIST) {
    return false;
  }

  if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    return false;
  }

  strncpy(runtimeAllowlist[runtimeAllowlistCount], address, 17);
  runtimeAllowlist[runtimeAllowlistCount][17] = '\0';
  runtimeAllowlistCount++;

  for (int i = 0; i < deviceIndex; i++) {
    if (strcmp(trackedDevices[i].address, address) == 0) {
      for (int j = i; j < deviceIndex - 1; j++) {
        trackedDevices[j] = trackedDevices[j + 1];
      }
      deviceIndex--;
      break;
    }
  }
  scrollIndex = 0;

  xSemaphoreGive(deviceMutex);
  saveRuntimeAllowlist();
  return true;
}

void toggleBrightness() {
  highBrightness = !highBrightness;
  M5.Display.setBrightness(highBrightness ? 204 : 77);
  lastButtonPressTime = millis();
  saveUserPreferences();
}

// Deliberate, user-triggered snapshot of currently-alerting devices — distinct
// from the passive per-scan logging removed earlier. Appends so multiple
// incidents across a session (or across power cycles) build a record.
// No RTC/NTP on this device, so timestamps are uptime-relative, not wall-clock.
int exportIncident() {
  if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    return 0;
  }

  File file = SPIFFS.open("/incidents.txt", FILE_APPEND);
  if (!file) {
    xSemaphoreGive(deviceMutex);
    return 0;
  }

  file.print("=== Incident export at uptime ");
  file.print(millis() / 1000);
  file.println("s ===");

  int count = 0;
  for (int i = 0; i < deviceIndex; i++) {
    if (!trackedDevices[i].detected) continue;
    file.print(trackedDevices[i].address);
    file.print(",");
    file.print(strlen(trackedDevices[i].name) > 0 ? trackedDevices[i].name : "Unknown");
    file.print(",");
    file.print(trackedDevices[i].manufacturer);
    file.print(",");
    file.print(trackerTypeName(trackedDevices[i].trackerType));
    file.print(",score=");
    file.print(trackedDevices[i].persistenceScore, 2);
    file.print(",firstSeenUptime=");
    file.print(trackedDevices[i].firstSeen);
    file.print(",totalCount=");
    file.println(trackedDevices[i].totalCount);
    count++;
  }

  // WiFi privacy-invader hits belong in the same snapshot — an export that
  // silently dropped them would misrepresent what the device actually saw.
  for (int i = 0; i < wifiDeviceIndex; i++) {
    if (!wifiDevices[i].isSpecial) continue;
    char mfg[31];
    getManufacturer(wifiDevices[i].bssid, mfg, sizeof(mfg));
    file.print(wifiDevices[i].bssid);
    file.print(",");
    file.print(strlen(wifiDevices[i].ssid) > 0 ? wifiDevices[i].ssid : "(hidden SSID)");
    file.print(",");
    file.print(mfg);
    file.print(",");
    file.print(trackerTypeName(TRACKER_PRIVACY_INVADER));
    file.print(",band=wifi,channel=");
    file.print(wifiDevices[i].channel);
    file.print(",rssi=");
    file.print(wifiDevices[i].rssi);
    file.print(",totalCount=");
    file.println(wifiDevices[i].detectionCount);
    count++;
  }

  if (count == 0) {
    file.println("(no currently-alerting devices)");
  }

  file.close();
  xSemaphoreGive(deviceMutex);
  return count;
}

void clearDevices() {
  if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    return;
  }

  deviceIndex = 0;
  // The WiFi list goes too. Alerting entries are deliberately pinned against
  // expiry now, so leaving them here would make "Clear Devices" visibly not
  // clear the rows most worth clearing.
  wifiDeviceIndex = 0;
  knownDeviceCount = 0;
  scrollIndex = 0;
  topVisibleValid = false;
  unackedAlertCount = 0;

  xSemaphoreGive(deviceMutex);
}

// Serial retrieval for exported incidents — send "dump" over Serial Monitor
// (115200 baud, newline line ending) to print /incidents.txt. No WiFi/USB-storage
// export path exists (deliberately — see the serial command console below for why).
void dumpIncidentsToSerial() {
  File file = SPIFFS.open("/incidents.txt", FILE_READ);
  if (!file) {
    Serial.println("No incidents exported yet.");
    return;
  }
  Serial.println("--- BEGIN /incidents.txt ---");
  while (file.available()) {
    Serial.write(file.read());
  }
  Serial.println("--- END /incidents.txt ---");
  file.close();
}

// No-reflash configuration console. A line-based command protocol over the
// same USB-serial connection Serial Monitor already uses for the "dump"
// command — deliberately not a WiFi-AP config page: this is an anti-stalking
// device, and Phase 2's "Quiet mode" exists specifically so it doesn't draw
// attention, so it shouldn't itself broadcast a discoverable SSID. Serial
// requires physical USB access and stays silent on RF.
//
// Requires the Serial Monitor's line ending set to "Newline" (or "Both NL &
// CR") — commands are dispatched on \n, not per keystroke.
void printSerialHelp() {
  Serial.println("--- PathShield serial commands ---");
  Serial.println("  help                             Show this list");
  Serial.println("  controls                         Show the on-device button controls");
  Serial.println("  dump                             Print /incidents.txt");
  Serial.println("  config                           Show full current configuration");
  Serial.println("  special list                     List privacy-invader MAC prefixes");
  Serial.println("  special add <prefix>              e.g. special add 00:25:DF");
  Serial.println("  special remove <index|prefix>     Remove by index (from 'special list') or exact text");
  Serial.println("  special reset                     Reset to compiled-in defaults");
  Serial.println("  allow list                       List runtime allowlist (exact MACs)");
  Serial.println("  allow add <MAC>                   e.g. allow add AA:BB:CC:DD:EE:FF");
  Serial.println("  allow remove <MAC>                Remove a MAC from the allowlist");
  Serial.println("  threshold list                   Show sensitivity thresholds");
  Serial.println("  threshold set <name> <value>      name: persistence | rssi_stability | rssi_variation");
  Serial.println("  threshold reset                   Reset thresholds to defaults");
  Serial.println("All changes are persisted to SPIFFS immediately — no reflash needed.");
}

void handleSpecialCommand(const char *sub, const char *arg) {
  if (!sub || strcasecmp(sub, "list") == 0) {
    Serial.println("--- Special (privacy-invader) MAC prefixes ---");
    for (int i = 0; i < specialMacsCount; i++) {
      Serial.printf("  [%d] %s\n", i, specialMacs[i]);
    }
    if (specialMacsCount == 0) Serial.println("  (none)");
  } else if (strcasecmp(sub, "add") == 0) {
    if (!arg || strlen(arg) < 2) {
      Serial.println("Usage: special add <MAC-prefix, e.g. 00:25:DF>");
      return;
    }
    if (addSpecialMac(arg)) {
      Serial.printf("Added special MAC prefix: %s\n", arg);
    } else {
      Serial.println("Failed to add (list full or device busy) — try again.");
    }
  } else if (strcasecmp(sub, "remove") == 0) {
    if (!arg) {
      Serial.println("Usage: special remove <index|prefix>");
      return;
    }
    int idx = -1;
    bool numeric = true;
    for (const char *p = arg; *p; p++) {
      if (*p < '0' || *p > '9') { numeric = false; break; }
    }
    if (numeric) {
      idx = atoi(arg);
    } else {
      for (int i = 0; i < specialMacsCount; i++) {
        if (strcasecmp(specialMacs[i], arg) == 0) { idx = i; break; }
      }
    }
    if (removeSpecialMac(idx)) {
      Serial.println("Removed.");
    } else {
      Serial.println("Not found.");
    }
  } else if (strcasecmp(sub, "reset") == 0) {
    resetSpecialMacsToDefault();
    saveSpecialMacs();
    Serial.println("Special MAC list reset to compiled-in defaults.");
  } else {
    Serial.println("Usage: special <list|add|remove|reset> [value]");
  }
}

void handleAllowCommand(const char *sub, const char *arg) {
  if (!sub || strcasecmp(sub, "list") == 0) {
    Serial.println("--- Runtime allowlist (exact MAC) ---");
    for (int i = 0; i < runtimeAllowlistCount; i++) {
      Serial.printf("  [%d] %s\n", i, runtimeAllowlist[i]);
    }
    if (runtimeAllowlistCount == 0) Serial.println("  (none)");
  } else if (strcasecmp(sub, "add") == 0) {
    if (!arg || strlen(arg) != 17) {
      Serial.println("Usage: allow add <AA:BB:CC:DD:EE:FF> (full MAC, exact match)");
      return;
    }
    if (allowlistDevice(arg)) {
      Serial.printf("Allowlisted: %s\n", arg);
    } else {
      Serial.println("Failed to add (list full or device busy) — try again.");
    }
  } else if (strcasecmp(sub, "remove") == 0) {
    if (!arg) {
      Serial.println("Usage: allow remove <AA:BB:CC:DD:EE:FF>");
      return;
    }
    if (removeFromAllowlist(arg)) {
      Serial.println("Removed.");
    } else {
      Serial.println("Not found.");
    }
  } else {
    Serial.println("Usage: allow <list|add|remove> [MAC]");
  }
}

void handleThresholdCommand(const char *sub, const char *name, const char *value) {
  if (!sub || strcasecmp(sub, "list") == 0 || strcasecmp(sub, "show") == 0) {
    Serial.println("--- Sensitivity thresholds ---");
    Serial.printf("  persistence     = %.2f  (0.0-1.0, alert once persistence score reaches this)\n",
                  persistenceThreshold);
    Serial.printf("  rssi_stability  = %d dBm  (delta at/below this counts a reading as 'stable')\n",
                  rssiStabilityThreshold);
    Serial.printf("  rssi_variation  = %d dBm  (delta at/above this counts as sketchy variance)\n",
                  rssiVariationThreshold);
    return;
  }
  if (strcasecmp(sub, "reset") == 0) {
    if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      persistenceThreshold = DEFAULT_PERSISTENCE_THRESHOLD;
      rssiStabilityThreshold = DEFAULT_RSSI_STABILITY_THRESHOLD;
      rssiVariationThreshold = DEFAULT_RSSI_VARIATION_THRESHOLD;
      xSemaphoreGive(deviceMutex);
    }
    saveUserPreferences();
    Serial.println("Thresholds reset to defaults.");
    return;
  }
  if (strcasecmp(sub, "set") == 0) {
    if (!name || !value) {
      Serial.println("Usage: threshold set <persistence|rssi_stability|rssi_variation> <value>");
      return;
    }
    if (strcasecmp(name, "persistence") == 0) {
      float v = atof(value);
      if (v < 0.0f || v > 1.0f) {
        Serial.println("persistence must be between 0.0 and 1.0");
        return;
      }
      if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        persistenceThreshold = v;
        xSemaphoreGive(deviceMutex);
      }
    } else if (strcasecmp(name, "rssi_stability") == 0) {
      int v = atoi(value);
      if (v < 1 || v > 50) {
        Serial.println("rssi_stability must be between 1 and 50");
        return;
      }
      if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        rssiStabilityThreshold = v;
        xSemaphoreGive(deviceMutex);
      }
    } else if (strcasecmp(name, "rssi_variation") == 0) {
      int v = atoi(value);
      if (v < 1 || v > 50) {
        Serial.println("rssi_variation must be between 1 and 50");
        return;
      }
      if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        rssiVariationThreshold = v;
        xSemaphoreGive(deviceMutex);
      }
    } else {
      Serial.println("Unknown threshold. Use: persistence, rssi_stability, rssi_variation");
      return;
    }
    saveUserPreferences();
    Serial.printf("Set %s = %s\n", name, value);
    return;
  }
  Serial.println("Usage: threshold <list|set|reset> ...");
}

void printSerialConfig() {
  Serial.println("=== PathShield configuration ===");
  Serial.printf("  alert style     = %s  (LOUD strobes, SIMPLE fills, QUIET borders)\n",
                alertStyleName(alertStyle));
  Serial.printf("  alert sound     = %s\n", alertSoundEnabled ? "ON" : "OFF");
  Serial.printf("  signal display  = %s\n", showSignalBars ? "BARS" : "dBm");
  handleSpecialCommand("list", NULL);
  handleAllowCommand("list", NULL);
  handleThresholdCommand("list", NULL, NULL);
}

// Dispatches one already-trimmed, null-terminated command line. Tokenizes with
// strtok in place, so `line` must be a writable buffer (not a string literal).
void handleSerialCommand(char *line) {
  char *cmd = strtok(line, " ");
  if (!cmd) return;

  if (strcasecmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    printSerialHelp();
  } else if (strcasecmp(cmd, "controls") == 0) {
    printControlsToSerial();
  } else if (strcasecmp(cmd, "dump") == 0 || strcasecmp(cmd, "d") == 0) {
    dumpIncidentsToSerial();
  } else if (strcasecmp(cmd, "config") == 0) {
    printSerialConfig();
  } else if (strcasecmp(cmd, "special") == 0) {
    char *sub = strtok(NULL, " ");
    char *arg = strtok(NULL, " ");
    handleSpecialCommand(sub, arg);
  } else if (strcasecmp(cmd, "allow") == 0) {
    char *sub = strtok(NULL, " ");
    char *arg = strtok(NULL, " ");
    handleAllowCommand(sub, arg);
  } else if (strcasecmp(cmd, "threshold") == 0) {
    char *sub = strtok(NULL, " ");
    char *name = strtok(NULL, " ");
    char *value = strtok(NULL, " ");
    handleThresholdCommand(sub, name, value);
  } else {
    Serial.printf("Unknown command '%s'. Type 'help' for commands.\n", cmd);
  }
}

void shutdownDevice() {
  M5.Display.fillScreen(RED);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(WHITE);
  M5.Display.setCursor(20, 50);
  M5.Display.print("Shutting Down");
  M5.Display.setTextSize(1);
  M5.Display.setCursor(40, 80);
  M5.Display.print("Goodbye!");
  delay(2000);
  M5.Power.powerOff();
}

void executeMenuOption(int index) {
  switch (index) {
    case 0:
      cycleAlertStyle();
      break;
    case 1:
      toggleAlertSound();
      break;
    case 2:
      toggleBrightness();
      showFeedback(highBrightness ? "BRIGHT" : "DIM", CYAN);
      delay(1000);
      break;
    case 3:
      cycleScreenTimeout();
      break;
    case 4:
      toggleSignalDisplay();
      break;
    case 5:
      allowlistTopDevice();
      break;
    case 6: {
      int exported = exportIncident();
      char msg[16];
      snprintf(msg, sizeof(msg), "%d EXPORTED", exported);
      showFeedback(msg, exported > 0 ? GREEN : DARKGREY);
      delay(1000);
      break;
    }
    case 7:
      clearDevices();
      showFeedback("CLEARED", GREEN);
      delay(1000);
      break;
    case 8:
      showControlsScreen(0);
      break;
    case 9:
      shutdownDevice();
      return;
  }

  forceDisplayRefresh();
  displayMenuScreen();
}

void cycleScreenTimeout() {
  int timeoutOptions[] = {10000, 15000, 30000, 60000, 120000, 300000};
  int optionCount = 6;
  int currentIdx = 0;

  for (int i = 0; i < optionCount; i++) {
    if (timeoutOptions[i] == screenTimeoutMs) {
      currentIdx = i;
      break;
    }
  }

  currentIdx = (currentIdx + 1) % optionCount;
  screenTimeoutMs = timeoutOptions[currentIdx];
  saveUserPreferences();

  char timeStr[20];
  snprintf(timeStr, 20, "%ds TIMEOUT", screenTimeoutMs / 1000);
  showFeedback(timeStr, CYAN);
  delay(1000);
}

// 3-state cycle: Simple -> Quiet -> Loud -> repeat. Sound is its own menu row
// now; folding it in here made a 4-state cycle nobody could predict.
void cycleAlertStyle() {
  alertStyle = (AlertStyle)((alertStyle + 1) % 3);
  saveUserPreferences();

  uint16_t color = alertStyle == ALERT_QUIET ? CYAN
                  : alertStyle == ALERT_LOUD ? RED
                  : ORANGE;
  showFeedback(alertStyleName(alertStyle), color,
               alertStyle == ALERT_QUIET  ? "Border only, no fill"
               : alertStyle == ALERT_LOUD ? "Red/blue strobe"
                                          : "Solid screen, no flashing");
  delay(1200);
}

void toggleSignalDisplay() {
  showSignalBars = !showSignalBars;
  saveUserPreferences();
  showFeedback(showSignalBars ? "BARS" : "dBm", CYAN,
               showSignalBars ? "Signal as 4-bar glyph" : "Signal as raw dBm");
  delay(1000);
}

void toggleAlertSound() {
  alertSoundEnabled = !alertSoundEnabled;
  saveUserPreferences();
  showFeedback(alertSoundEnabled ? "SOUND ON" : "SOUND OFF",
               alertSoundEnabled ? GREEN : DARKGREY);
  delay(1000);
}

void allowlistTopDevice() {
  // Acts on the topmost row of the last-rendered BLE list — kills a false
  // positive on the spot, no reflash needed. BLE-only (see topVisibleValid).
  if (!topVisibleValid) {
    showFeedback("NOTHING", DARKGREY, "No BLE device on the list");
    delay(1200);
    return;
  }

  char allowedAddr[18];
  strncpy(allowedAddr, topVisibleAddress, 17);
  allowedAddr[17] = '\0';
  if (allowlistDevice(allowedAddr)) {
    // The device is gone from the tracked list now; the next render re-points
    // this at whatever moved up into its place.
    topVisibleValid = false;
    showFeedback("ALLOWED", GREEN, allowedAddr);
  } else {
    showFeedback("LIST FULL", RED);
  }
  delay(1200);
}

void forceDisplayRefresh() {
  lastStateHash = 0;
  lastDisplayRender = 0;
  lastMenuRender = 0;
}

void refreshList() {
  forceDisplayRefresh();
  displayTrackedDevices();
}

// Blocks while the button stays down, up to BUTTON_HOLD_MS, and reports
// whether it was still down at the end. A tap returns immediately on release,
// so short-press actions stay snappy — only an actual hold costs the wait.
// Core 1 only: it drives M5.update().
bool waitForHold(m5::Button_Class &btn) {
  unsigned long pressStart = millis();
  while (btn.isPressed() && (millis() - pressStart < BUTTON_HOLD_MS)) {
    M5.update();
    delay(10);
  }
  return (millis() - pressStart >= BUTTON_HOLD_MS);
}

void setPaused(bool wantPaused) {
  paused = wantPaused;
  scrollIndex = 0;
  showFeedback(paused ? "STOPPED" : "SCANNING", paused ? RED : GREEN,
               paused ? "Device detail - A steps through" : NULL);
  delay(900);
  refreshList();
}

void openMenu() {
  inMenu = true;
  menuIndex = 0;
  lastActivityTime = millis();
  forceDisplayRefresh();
  displayMenuScreen();
}

void closeMenu() {
  inMenu = false;
  lastActivityTime = millis();
  refreshList();
}

void cycleFilter() {
  filterMode = (FilterMode)((filterMode + 1) % 3);

  // Looking at the alerts is the acknowledgement — the red border and its count
  // have done their job once the user is on the screen that lists what fired.
  if (filterMode == FILTER_ALERTS) {
    unackedAlertCount = 0;
  }

  // Row counts differ per filter; keeping the old offset would drop the user
  // into the middle of a list they didn't scroll.
  scrollIndex = 0;

  const char *label = filterMode == FILTER_NAMED ? "NAMED"
                     : filterMode == FILTER_ALERTS ? "ALERTS"
                     : "ALL";
  showFeedback(label, filterColor(filterMode),
               filterMode == FILTER_NAMED ? "BLE only" : "WiFi + BLE");
  delay(800);
  refreshList();
}

// One scroll direction that wraps, rather than separate up/down buttons —
// Button B is needed for the filter. Advances a page at a time rather than a
// row: at three rows per screen, stepping by one meant 67 taps to walk a full
// 70-device list, and each tap re-rendered two rows the user had already read.
void scrollList() {
  // One device per step in the detail view, a whole page in the list view.
  const int step = isDetailView() ? 1 : 3;
  int maxTop = lastVisibleItemCount - step;
  if (maxTop < 0) maxTop = 0;
  scrollIndex = (scrollIndex >= maxTop) ? 0 : scrollIndex + step;
  if (scrollIndex > maxTop) scrollIndex = maxTop;
  refreshList();
}

void handleBtnA() {
  lastButtonPressTime = millis();
  lastActivityTime = millis();

  if (waitForHold(M5.BtnA)) {
    setPaused(!paused);
  } else {
    scrollList();
  }
}

void handleBtnB() {
  lastButtonPressTime = millis();
  lastActivityTime = millis();

  if (waitForHold(M5.BtnB)) {
    openMenu();
  } else {
    cycleFilter();
  }
}

// SCANNING TASK - Runs on Core 0
void scanTask(void *parameter) {
  Serial.println("scanTask started on Core 0");

  vTaskDelay(1000 / portTICK_PERIOD_MS);
  Serial.println("scanTask beginning scans");

  unsigned long lastScanSwitch = 0;
  const unsigned long SCAN_SWITCH_INTERVAL = 3000;
  bool localScanningWiFi = true;

  while (scanTaskRunning) {
    if (paused || showingAlert) {
      esp_task_wdt_reset();
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    unsigned long currentMillis = millis();
    unsigned long currentTime = currentMillis / 1000;

    // Switch between WiFi and BLE scanning
    if (currentMillis - lastScanSwitch > SCAN_SWITCH_INTERVAL) {
      localScanningWiFi = !localScanningWiFi;
      lastScanSwitch = currentMillis;

      // Update global flag with mutex (with timeout to prevent deadlock)
      if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        scanningWiFi = localScanningWiFi;
        xSemaphoreGive(deviceMutex);
      }
    }

    // A single scan batch can surface several distinct new trackers at once
    // (e.g. walking past a shelf of AirTags). Queue every one of them here
    // instead of only ever showing the last — trackedDevices[0] is only *this*
    // device right when trackDevice() returns true for it (it just moved
    // itself there), so snapshot immediately or a later device in the same
    // batch overwrites it and the alert is lost. Shared by both bands since
    // WiFi raises privacy-invader alerts too; drained below, outside the mutex.
    PendingAlertInfo alertQueue[8];
    int queuedAlerts = 0;

    if (localScanningWiFi) {
      // WiFi Scan (blocking)
      int n = WiFi.scanNetworks(false, false, false, 300);

      if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        for (int i = 0; i < n; i++) {
          // Copied out rather than aliased: WiFi.BSSIDstr()/SSID() each return
          // a temporary String, and a pointer into one dangles the moment the
          // full expression ends.
          char bssid[18];
          char ssid[33];
          strncpy(bssid, WiFi.BSSIDstr(i).c_str(), 17);
          bssid[17] = '\0';
          strncpy(ssid, WiFi.SSID(i).c_str(), 32);
          ssid[32] = '\0';

          // The allowlist applies to BSSIDs too. Without this a WiFi false
          // positive would have no off switch at all — neither the menu action
          // nor `allow add` could silence it.
          if (isAllowlistedMac(bssid)) {
            vTaskDelay(1 / portTICK_PERIOD_MS);
            continue;
          }

          bool isNewInvader = trackWiFiDevice(ssid, bssid, WiFi.RSSI(i),
                                              WiFi.channel(i),
                                              WiFi.encryptionType(i), currentTime);

          if (isNewInvader && queuedAlerts < 8) {
            alertQueue[queuedAlerts].isSpecial = true;
            strncpy(alertQueue[queuedAlerts].name,
                    strlen(ssid) > 0 ? ssid : "(hidden SSID)", 20);
            alertQueue[queuedAlerts].name[20] = '\0';
            strncpy(alertQueue[queuedAlerts].mac, bssid, 17);
            alertQueue[queuedAlerts].mac[17] = '\0';
            // No persistence scoring on this path — a matching OUI is the whole
            // finding, so report full confidence rather than a meaningless 0.00.
            alertQueue[queuedAlerts].score = 1.0f;
            alertQueue[queuedAlerts].trackerType = TRACKER_PRIVACY_INVADER;
            alertQueue[queuedAlerts].isWifi = true;
            queuedAlerts++;
          }
          vTaskDelay(1 / portTICK_PERIOD_MS);
        }
        xSemaphoreGive(deviceMutex);
      }
      WiFi.scanDelete();

    } else {
      // BLE Scan (blocking)
      NimBLEScanResults foundDevices = pBLEScan->getResults(2000, false);

      {
        if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
          int count = foundDevices.getCount();
          for (int i = 0; i < count; i++) {
            const NimBLEAdvertisedDevice *device = foundDevices.getDevice(i);
            if (!device) continue;
            char macAddr[18];
            strncpy(macAddr, device->getAddress().toString().c_str(), 17);
            macAddr[17] = '\0';

            // Skip weak signals — not worth tracking
            if (device->getRSSI() < RSSI_FLOOR) continue;

            if (!isAllowlistedMac(macAddr)) {
              uint8_t tType = detectTrackerType(*device);
              bool isNewTracker = false;

              // Known trackers bypass the known-device buffer entirely
              if (tType != TRACKER_NONE) {
                isNewTracker = trackDevice(macAddr, device->getRSSI(), currentTime,
                                           device->getName().c_str(), tType);
              } else if (!handleKnownDevice(macAddr, device->getRSSI(), currentTime)) {
                isNewTracker = trackDevice(macAddr, device->getRSSI(), currentTime,
                                           device->getName().c_str());
              }

              if (isNewTracker && deviceIndex > 0 && queuedAlerts < 8) {
                DeviceInfo &d = trackedDevices[0];
                alertQueue[queuedAlerts].isSpecial = d.isSpecial;
                strncpy(alertQueue[queuedAlerts].name, d.name, 20);
                alertQueue[queuedAlerts].name[20] = '\0';
                strncpy(alertQueue[queuedAlerts].mac, d.address, 17);
                alertQueue[queuedAlerts].mac[17] = '\0';
                alertQueue[queuedAlerts].score = d.persistenceScore;
                alertQueue[queuedAlerts].trackerType = d.trackerType;
                alertQueue[queuedAlerts].isWifi = false;
                queuedAlerts++;
              }
            }
            vTaskDelay(1 / portTICK_PERIOD_MS);
          }
          xSemaphoreGive(deviceMutex);
        }

        pBLEScan->clearResults();
      }
    }

    // Show every queued alert in turn, outside the mutex — from either band.
    for (int q = 0; q < queuedAlerts; q++) {
      pendingAlertInfo = alertQueue[q];
      showingAlert = true;
      alertPending = true;

      // Hold here until loop() (Core 1) has shown the alert and it has been
      // dismissed — by a button or by its own timeout. Keep feeding the
      // watchdog: this wait is expected to last as long as the alert is up.
      while (showingAlert && scanTaskRunning) {
        esp_task_wdt_reset();
        vTaskDelay(50 / portTICK_PERIOD_MS);
      }
    }

    // Clean up old entries periodically (also promotes stable devices to known)
    if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      removeOldEntries(currentTime);
      removeOldWiFiEntries(currentTime);
      removeOldKnownEntries(currentTime);
      xSemaphoreGive(deviceMutex);
    }

    esp_task_wdt_reset();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }

  Serial.println("scanTask terminated");
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("PathShield v" FIRMWARE_VERSION " — starting setup...");

  M5.begin();
  Serial.println("M5 initialized");
  delay(100);

  M5.Display.fillScreen(BLACK);
  M5.Display.setRotation(3);
  M5.Display.setTextColor(GREEN);
  M5.Display.setTextSize(1);
  // Don't set brightness here - will be set after loading preferences
  M5.Display.setBrightness(204); // Temporary for startup message

  delay(100);
  Serial.println("Display initialized");

  displayStartupMessage();
  Serial.println("Startup message displayed");

  delay(100);

  Serial.printf("Heap before BLE init: %dKB\n", ESP.getFreeHeap() / 1024);

  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(new MyScanCallbacks());
  pBLEScan->setInterval(1100);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->setDuplicateFilter(true);
  pBLEScan->start(0, false);

  Serial.printf("Heap after BLE init: %dKB\n", ESP.getFreeHeap() / 1024);
  delay(200);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  Serial.printf("Heap after WiFi init: %dKB\n", ESP.getFreeHeap() / 1024);

  // M5StickS3 always ships with 8MB OPI PSRAM. If it's missing, the board's
  // PSRAM setting wasn't enabled correctly at build time — fail loudly
  // instead of silently degrading to a reduced-capacity heap-only mode.
  hasPsram = psramFound();
  if (!hasPsram) {
    Serial.println("FATAL: No PSRAM detected. Enable 'OPI PSRAM' in board settings and reflash.");
    M5.Display.fillScreen(RED);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(10, 40);
    M5.Display.print("NO PSRAM");
    M5.Display.setTextSize(1);
    M5.Display.setCursor(10, 70);
    M5.Display.print("Enable OPI PSRAM in board");
    M5.Display.setCursor(10, 82);
    M5.Display.print("settings, then reflash");
    while (1) delay(1000);
  }

  maxDevices = MAX_DEVICES_CAP;
  maxWifiDevices = MAX_WIFI_DEVICES_CAP;
  maxKnownDevices = MAX_KNOWN_CAP;
  Serial.printf("PSRAM detected: %dKB — device limits: %d BLE, %d WiFi, %d Known\n",
                ESP.getPsramSize() / 1024, maxDevices, maxWifiDevices, maxKnownDevices);

  trackedDevices = (DeviceInfo *)ps_malloc(maxDevices * sizeof(DeviceInfo));
  wifiDevices = (WiFiDeviceInfo *)ps_malloc(maxWifiDevices * sizeof(WiFiDeviceInfo));
  knownDevices = (KnownDevice *)ps_malloc(maxKnownDevices * sizeof(KnownDevice));
  memset(trackedDevices, 0, maxDevices * sizeof(DeviceInfo));
  memset(wifiDevices, 0, maxWifiDevices * sizeof(WiFiDeviceInfo));
  memset(knownDevices, 0, maxKnownDevices * sizeof(KnownDevice));
  Serial.printf("Device arrays allocated: BLE=%dKB WiFi=%dKB Known=%dKB | Heap remaining: %dKB\n",
                (maxDevices * sizeof(DeviceInfo)) / 1024,
                (maxWifiDevices * sizeof(WiFiDeviceInfo)) / 1024,
                (maxKnownDevices * sizeof(KnownDevice)) / 1024,
                ESP.getFreeHeap() / 1024);
  initialFreeHeapKB = ESP.getFreeHeap() / 1024;

  // Frame buffer for the repainting screens — PSRAM-backed, so it stays out of
  // the internal heap measured just above and reported by the MEM bar. Failing
  // to allocate is not fatal: frame() falls back to drawing straight to the
  // panel, which is exactly the pre-sprite behaviour.
  frameCanvas.setPsram(true);
  frameCanvas.setColorDepth(16);
  canvasReady = (frameCanvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT) != nullptr);
  Serial.printf("Frame canvas: %s (%dKB PSRAM)\n",
                canvasReady ? "ready" : "UNAVAILABLE — falling back to direct draw",
                (SCREEN_WIDTH * SCREEN_HEIGHT * 2) / 1024);

  if (!SPIFFS.begin(false)) {
    Serial.println("SPIFFS corrupted, formatting...");

    M5.Display.fillScreen(BLACK);
    for (int i = 0; i < 15; i++) {
      M5.Display.drawFastHLine(random(0, SCREEN_WIDTH), random(0, SCREEN_HEIGHT),
                               random(5, 40), random(0x0000, 0x1111));
    }

    M5.Display.drawFastHLine(0, 25, SCREEN_WIDTH, MAGENTA);
    M5.Display.drawFastHLine(0, 26, SCREEN_WIDTH, MAGENTA);

    M5.Display.setTextSize(3);
    M5.Display.setTextColor(YELLOW);
    M5.Display.setCursor(25, 35);
    M5.Display.print("SPIFFS");

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(ORANGE);
    M5.Display.setCursor(55, 60);
    M5.Display.print("FORMATTING...");

    M5.Display.drawFastHLine(0, 75, SCREEN_WIDTH, CYAN);
    M5.Display.drawFastHLine(0, 76, SCREEN_WIDTH, CYAN);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(DARKGREY);
    M5.Display.setCursor(35, 90);
    M5.Display.print("Wait 60s...");

    SPIFFS.format();
    delay(100);

    if (!SPIFFS.begin(false)) {
      Serial.println("SPIFFS mount failed critically");
      M5.Display.fillScreen(BLACK);

      for (int i = 0; i < 20; i++) {
        M5.Display.drawFastHLine(random(0, SCREEN_WIDTH), random(0, SCREEN_HEIGHT),
                                 random(5, 40), random(0x0000, 0x1111));
      }

      M5.Display.drawFastHLine(0, 25, SCREEN_WIDTH, RED);
      M5.Display.drawFastHLine(0, 26, SCREEN_WIDTH, RED);

      M5.Display.setTextSize(3);
      M5.Display.setTextColor(RED);
      M5.Display.setCursor(40, 40);
      M5.Display.print("ERROR");

      M5.Display.setTextSize(1);
      M5.Display.setTextColor(YELLOW);
      M5.Display.setCursor(30, 70);
      M5.Display.print("SPIFFS FAILED");

      M5.Display.drawFastHLine(0, 85, SCREEN_WIDTH, RED);
      M5.Display.drawFastHLine(0, 86, SCREEN_WIDTH, RED);

      M5.Display.setTextColor(DARKGREY);
      M5.Display.setCursor(50, 100);
      M5.Display.print("Reboot needed");

      while(1) delay(1000);
      return;
    }

    // Show success message
    M5.Display.fillScreen(BLACK);
    for (int i = 0; i < 15; i++) {
      M5.Display.drawFastHLine(random(0, SCREEN_WIDTH), random(0, SCREEN_HEIGHT),
                               random(5, 40), random(0x0000, 0x1111));
    }
    M5.Display.drawFastHLine(0, 35, SCREEN_WIDTH, GREEN);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(GREEN);
    M5.Display.setCursor(35, 50);
    M5.Display.print("SUCCESS!");
    M5.Display.drawFastHLine(0, 70, SCREEN_WIDTH, GREEN);
    delay(800);
  }

  Serial.println("SPIFFS initialized");

  // Load user preferences (brightness, timeout, etc.)
  loadUserPreferences();
  currentlyBright = highBrightness;
  Serial.print("User preferences loaded - Brightness: ");
  Serial.println(highBrightness ? "High" : "Low");

  loadRuntimeAllowlist();
  Serial.printf("Runtime allowlist loaded: %d device(s)\n", runtimeAllowlistCount);

  loadSpecialMacs();
  Serial.printf("Special MAC prefixes loaded: %d\n", specialMacsCount);
  Serial.printf("Thresholds — persistence: %.2f, rssi_stability: %d, rssi_variation: %d\n",
                persistenceThreshold, rssiStabilityThreshold, rssiVariationThreshold);
  Serial.printf("Alert style: %s, sound %s\n", alertStyleName(alertStyle),
                alertSoundEnabled ? "on" : "off");
  printControlsToSerial();
  Serial.println("Type 'help' over serial for the no-reflash config console.");

  // Controls sheet before the first scan — the button scheme is the one thing
  // a new user can't work out by looking at the findings screen. Dismissable,
  // and available any time from the settings menu.
  M5.Display.setBrightness(highBrightness ? 204 : 77);
  showControlsScreen(6000);

  Serial.print("Screen Timeout: ");
  Serial.println(screenTimeoutMs);

  deviceMutex = xSemaphoreCreateMutex();
  if (deviceMutex == NULL) {
    Serial.println("ERROR: Failed to create device mutex!");
    while (1) { delay(300); }
  }
  Serial.println("Device mutex created");
  
  delay(1000);

  // Composed through the frame like every other top-bar render, so this first
  // screen matches what the list draws a moment later.
  {
    auto &gfx = frame();
    gfx.fillScreen(BLACK);
    drawTopBar();
    gfx.setTextSize(1);
    gfx.setTextColor(DARKGREY);
    gfx.setCursor(60, 60);
    gfx.print("Starting scans...");
    framePush();
  }
  Serial.println("Initial display ready");

  BaseType_t scanTaskCreated = xTaskCreatePinnedToCore(
    scanTask,
    "ScanTask",
    16384,
    NULL,
    1,
    &scanTaskHandle,
    0
  );
  if (scanTaskCreated != pdPASS || scanTaskHandle == NULL) {
    // Must halt here rather than continue: esp_task_wdt_add(NULL) below would
    // otherwise subscribe *this* task (Core 1 loop) to the watchdog, and
    // since loop() never resets it, that's a guaranteed reboot-loop.
    Serial.println("ERROR: Failed to create scanTask!");
    M5.Display.fillScreen(RED);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(10, 50);
    M5.Display.print("TASK INIT");
    M5.Display.setCursor(10, 75);
    M5.Display.print("FAILED");
    while (1) { delay(300); }
  }
  Serial.println("Scanning task started on Core 0");

  // Watchdog covers scanTask only (idle_core_mask=0 — don't watch idle tasks,
  // that's what made the old default WDT fire spuriously and got it disabled
  // entirely). A hung BLE/WiFi call now reboots the device instead of
  // freezing it forever; scanTask feeds it every loop iteration and while
  // legitimately waiting on `paused` or an on-screen alert.
  esp_task_wdt_config_t twdtConfig = {
    .timeout_ms = 20000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  if (esp_task_wdt_init(&twdtConfig) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&twdtConfig);
  }
  esp_task_wdt_add(scanTaskHandle);
  Serial.println("Task watchdog armed on scanTask (20s timeout)");

  delay(100);

  lastActivityTime = millis();
  lastButtonPressTime = millis();

  Serial.println("PathShield setup complete");
}

void loop() {
  unsigned long currentMillis = millis();
  static unsigned long lastDisplayUpdate = 0;
  static bool firstRun = true;
  static unsigned long lastMemoryCheck = 0;
  static char serialLineBuf[64];
  static size_t serialLineLen = 0;
  const unsigned long DISPLAY_UPDATE_INTERVAL = 1000;
  const unsigned long MEMORY_CHECK_INTERVAL = 5000;

  M5.update();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLineLen > 0) {
        serialLineBuf[serialLineLen] = '\0';
        handleSerialCommand(serialLineBuf);
        serialLineLen = 0;
      }
    } else if (serialLineLen < sizeof(serialLineBuf) - 1) {
      serialLineBuf[serialLineLen++] = c;
    }
  }

  // Tracker alert deposited by scanTask (Core 0) — render and wait for
  // dismissal here on Core 1, the only task allowed to touch the display/buttons.
  if (alertPending) {
    PendingAlertInfo local = pendingAlertInfo;
    alertPending = false;
    alertUser(local.isSpecial, local.name, local.mac, local.score,
              local.trackerType, local.isWifi);
    showingAlert = false;
    forceDisplayRefresh();
    lastDisplayUpdate = currentMillis;
    return;
  }

  // Critical battery. Checked here rather than inside drawTopBar(), where it
  // used to sit: a render function is the wrong place to power the device off,
  // and it did so while holding deviceMutex across a 3-second delay. Shares the
  // memory check's interval so it isn't sampled every loop iteration.
  if (currentMillis - lastMemoryCheck > MEMORY_CHECK_INTERVAL &&
      batteryPercent() <= 3) {
    M5.Display.setBrightness(204);
    M5.Display.fillScreen(RED);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(10, 50);
    M5.Display.print("LOW BATTERY!");
    M5.Display.setCursor(20, 80);
    M5.Display.print("SHUTTING DOWN");
    delay(3000);
    M5.Power.powerOff();
  }

  if (currentMillis - lastMemoryCheck > MEMORY_CHECK_INTERVAL) {
    lastMemoryCheck = currentMillis;
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t freeKB = freeHeap / 1024;

    if (freeKB < 10) {
      Serial.println("CRITICAL MEMORY! Forcing cleanup...");
      if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        int toClear = deviceIndex / 2;
        for (int i = deviceIndex - toClear; i < deviceIndex; i++) {
          if (!trackedDevices[i].alertTriggered && !trackedDevices[i].isSpecial) {
            deviceIndex = i;
            break;
          }
        }
        xSemaphoreGive(deviceMutex);
      }
    }
  }

  if (firstRun) {
    M5.Display.setBrightness(highBrightness ? 204 : 77);
    screenOn = true;
    delay(200);
    displayTrackedDevices();
    lastDisplayUpdate = currentMillis;
    firstRun = false;
    return;
  }

  bool btnA = M5.BtnA.wasPressed();
  bool btnB = M5.BtnB.wasPressed();

  // WAKE SCREEN ON ANY BUTTON
  if ((btnA || btnB) && !screenOn) {
    screenOn = true;
    screenDimmed = false;
    currentlyBright = highBrightness;
    lastActivityTime = currentMillis;
    M5.Display.setBrightness(highBrightness ? 204 : 77);
    lastStateHash = 0;
    lastDisplayRender = 0;
    lastMenuRender = 0;
    lastBtnAPress = currentMillis;
    lastBtnBPress = currentMillis;

    if (inMenu) {
      displayMenuScreen();
    } else {
      displayTrackedDevices();
    }
    lastDisplayUpdate = currentMillis;
    return;
  }

  if (btnA || btnB) {
    lastActivityTime = currentMillis;
    screenDimmed = false;
    currentlyBright = highBrightness;
    M5.Display.setBrightness(highBrightness ? 204 : 77);
  }

  // HANDLE INPUT BASED ON MODE
  //
  // Gestures are tap vs hold on a single button — no A+B chord. The chord was
  // the only way into the settings menu and was near-impossible to land, since
  // whichever button went down first had already fired its own action.
  // Handlers that hold-detect block for up to BUTTON_HOLD_MS, so re-stamp the
  // debounce timestamps afterwards against the clock as it is on return.
  if (screenOn) {
    if (inMenu) {
      // MENU MODE — A steps through options (no hold-detect, so repeated
      // presses stay instant), B selects or, held, closes the menu.
      if (btnA && (currentMillis - lastBtnAPress > DEBOUNCE_DELAY)) {
        lastBtnAPress = currentMillis;
        highlightMenuOption((menuIndex + 1) % MENU_OPTION_COUNT);
        lastActivityTime = currentMillis;
        return;
      }
      if (btnB && (currentMillis - lastBtnBPress > DEBOUNCE_DELAY)) {
        if (waitForHold(M5.BtnB)) {
          closeMenu();
        } else {
          executeMenuOption(menuIndex);
        }
        lastBtnBPress = millis();
        lastActivityTime = millis();
        lastDisplayUpdate = millis();
        return;
      }
    } else {
      // NORMAL MODE
      if (btnA && (currentMillis - lastBtnAPress > DEBOUNCE_DELAY)) {
        handleBtnA();
        lastBtnAPress = millis();
        lastActivityTime = millis();
        lastDisplayUpdate = millis();
        return;
      }
      if (btnB && (currentMillis - lastBtnBPress > DEBOUNCE_DELAY)) {
        handleBtnB();
        lastBtnBPress = millis();
        lastActivityTime = millis();
        lastDisplayUpdate = millis();
        return;
      }
    }
  }

  // SCREEN TIMEOUT LOGIC
  unsigned long dimThreshold = min(screenTimeoutMs * 3 / 4, screenTimeoutMs - 5000);
  if (dimThreshold < 5000) dimThreshold = screenTimeoutMs / 2;

  if (screenOn && !screenDimmed && currentlyBright &&
      currentMillis - lastActivityTime > dimThreshold) {
    currentlyBright = false;
    screenDimmed = true;
    M5.Display.setBrightness(10);
  }

  if (screenOn && currentMillis - lastActivityTime > screenTimeoutMs) {
    screenOn = false;
    M5.Display.setBrightness(0);
  }

  // PERIODIC DISPLAY UPDATE
  if (screenOn && currentMillis - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
    if (inMenu) {
      displayMenuScreen();
    } else {
      displayTrackedDevices();
    }
    lastDisplayUpdate = currentMillis;
  }

  vTaskDelay(50 / portTICK_PERIOD_MS);
}
