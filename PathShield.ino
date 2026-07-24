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
unsigned long lastComboCheck = 0;
const unsigned long DEBOUNCE_DELAY = 200;
bool inMenu = false;
int menuIndex = 0;
int menuBaseY = 0;
#define MENU_OPTION_COUNT 6

bool highBrightness = true;
bool currentlyBright = true;
bool paused = false;
enum FilterMode { FILTER_ALL, FILTER_NAMED, FILTER_ALERTS };
FilterMode filterMode = FILTER_ALL;
bool discreetAlerts = false;
bool alertSoundEnabled = true;
bool screenDimmed = false;
unsigned long lastButtonPressTime = 0;
unsigned long lastActivityTime = 0;
bool screenOn = true;
unsigned long screenTimeoutMs = DEFAULT_SCREEN_TIMEOUT;
volatile bool deviceDataChanged = false;
static uint32_t lastStateHash = 0;
unsigned long lastDisplayRender = 0;
unsigned long lastMenuRender = 0;

bool alertActive = false;
unsigned long alertStartTime = 0;
const unsigned long ALERT_DURATION = 5000;

// Alert handoff from scanTask (Core 0) to loop() (Core 1).
// All M5.Display / M5.update() calls must happen on Core 1 only — scanTask
// never touches display or button hardware directly, it just deposits data here.
struct PendingAlertInfo {
  bool isSpecial;
  char name[21];
  char mac[18];
  float score;
  uint8_t trackerType;
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

bool isSpecialMac(const char *address) {
  for (int i = 0; i < specialMacsCount; i++) {
    if (strncmp(address, specialMacs[i], strlen(specialMacs[i])) == 0) {
      return true;
    }
  }
  return false;
}

bool isAllowlistedMac(const char *address) {
  for (size_t i = 0; i < sizeof(allowlistMacs) / sizeof(allowlistMacs[0]); i++) {
    if (strlen(allowlistMacs[i]) > 0 &&
        strncmp(address, allowlistMacs[i], strlen(allowlistMacs[i])) == 0) {
      return true;
    }
  }
  for (int i = 0; i < runtimeAllowlistCount; i++) {
    if (strcmp(address, runtimeAllowlist[i]) == 0) {
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

void trackWiFiDevice(const char *ssid, const char *bssid, int rssi, int channel,
                     int encType, unsigned long currentTime) {
  for (int i = 0; i < wifiDeviceIndex; i++) {
    if (strcmp(wifiDevices[i].bssid, bssid) == 0) {
      wifiDevices[i].rssi = rssi;
      wifiDevices[i].lastSeen = currentTime;
      wifiDevices[i].detectionCount++;
      return;
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
    wifiDeviceIndex++;
  } else {
    int oldestIndex = -1;
    unsigned long oldestTime = currentTime;

    for (int i = 0; i < wifiDeviceIndex; i++) {
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
    }
  }
}

void removeOldWiFiEntries(unsigned long currentTime) {
  for (int i = 0; i < wifiDeviceIndex; i++) {
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
               float persistence, uint8_t tType = TRACKER_NONE) {

  screenOn = true;
  lastActivityTime = millis();
  // Loud mode always jumps to max brightness to grab attention. Discreet mode
  // respects whatever brightness the user already chose — the point is to
  // not draw attention, so don't force the screen bright either.
  M5.Display.setBrightness(discreetAlerts ? (highBrightness ? 204 : 77) : 204);

  if (alertSoundEnabled) {
    M5.Speaker.tone(1800, 120);
    delay(160);
    M5.Speaker.tone(1800, 120);
    delay(160);
  }

  if (discreetAlerts) {
    // Quiet variant: no strobe, just a black screen with a thin colored
    // border — enough to notice without drawing attention to the device.
    M5.Display.fillScreen(BLACK);
    M5.Display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, isSpecial ? ORANGE : RED);
    M5.Display.drawRect(1, 1, SCREEN_WIDTH - 2, SCREEN_HEIGHT - 2, isSpecial ? ORANGE : RED);
  } else if (isSpecial) {
    for (int i = 0; i < 5; i++) {
      M5.Display.fillScreen(RED);
      delay(200);
      M5.Display.fillScreen(BLUE);
      delay(200);
    }
  } else {
    M5.Display.fillScreen(RED);
  }

  M5.Display.setCursor(0, 10);
  M5.Display.setTextColor(discreetAlerts ? (isSpecial ? ORANGE : RED) : WHITE);
  M5.Display.setTextSize(2);
  M5.Display.print(discreetAlerts ? "Tracker Alert" : "Tracker Detected!");
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
  M5.Display.print("Name: ");
  M5.Display.print(strlen(name) > 0 ? name : "Unknown");

  M5.Display.setCursor(0, 70);
  M5.Display.print("MAC: ");
  M5.Display.print(mac);

  M5.Display.setCursor(0, 85);
  M5.Display.print("Score: ");
  M5.Display.print(persistence, 2);

  M5.Display.setCursor(0, 105);
  M5.Display.setTextColor(YELLOW);
  M5.Display.print("Press any button");

  while (!M5.BtnA.wasPressed() && !M5.BtnB.wasPressed()) {
    M5.update();
    delay(50);
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
  M5.Display.print("v2.2.0");

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

void drawTopBar() {
  M5.Display.drawFastHLine(0, 0, SCREEN_WIDTH, CYAN);
  M5.Display.drawFastHLine(0, 1, SCREEN_WIDTH, CYAN);

  M5.Display.setTextSize(1);
  M5.Display.setCursor(4, 3);
  M5.Display.setTextColor(paused ? RED : GREEN);
  M5.Display.print(paused ? "PAUSE" : (scanningWiFi ? "WiFi" : "BLE"));

  static float lastValidBatVoltage = 3.7f;
  float batVoltage = M5.Power.getBatteryVoltage() / 1000.0f;

  // Filter out obviously bad readings (sensor errors), but allow real low battery
  if (batVoltage > 2.0f && batVoltage < 4.5f) {
    lastValidBatVoltage = batVoltage;
  } else {
    batVoltage = lastValidBatVoltage;
  }

  // Critical battery check - force shutdown if critically low
  if (batVoltage < 3.0f && batVoltage > 2.0f) {
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

  int batPercent = (int)((batVoltage - 3.0f) / 1.2f * 100.0f);
  if (batPercent > 100) batPercent = 100;
  if (batPercent < 0) batPercent = 0;

  int barWidth = 30;
  int barX = 75;
  int barY = 4;

  uint16_t barColor = GREEN;
  if (batPercent < 50) barColor = YELLOW;
  if (batPercent < 25) barColor = RED;

  M5.Display.setTextColor(BLUE_GREY);
  M5.Display.setCursor(45, 3);
  M5.Display.print("BATT:");

  M5.Display.drawRect(barX, barY, barWidth, 6, BLUE_GREY);
  int filledWidth = (barWidth - 2) * batPercent / 100;
  M5.Display.fillRect(barX + 1, barY + 1, filledWidth, 4, barColor);

  M5.Display.setTextColor(DARKGREY);
  M5.Display.setCursor(110, 3);
  M5.Display.print(batPercent);
  M5.Display.print("%");

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

  M5.Display.setCursor(135, 3);
  M5.Display.setTextColor(BLUE_GREY);
  M5.Display.print("MEM:");

  M5.Display.drawRect(barX, barY, barWidth, 6, BLUE_GREY);
  filledWidth = (barWidth - 2) * memPercent / 100;
  M5.Display.fillRect(barX + 1, barY + 1, filledWidth, 4, memColor);

  M5.Display.setTextColor(memColor);
  M5.Display.setCursor(190, 3);
  M5.Display.print(freeKB);
  M5.Display.print("KB");

  M5.Display.drawFastHLine(0, 11, SCREEN_WIDTH, CYAN);
  M5.Display.drawFastHLine(0, 12, SCREEN_WIDTH, CYAN);
}

// Generate hashed display state
uint32_t getDisplayStateHash() {
  uint32_t hash = 1;

  hash = hash * 31 + deviceIndex;
  hash = hash * 31 + wifiDeviceIndex;
  hash = hash * 31 + scrollIndex;
  hash = hash * 31 + (scanningWiFi ? 1 : 0);
  hash = hash * 31 + (paused ? 1 : 0);
  hash = hash * 31 + (uint32_t)filterMode;

  if (scanningWiFi) {
    for (int i = 0; i < wifiDeviceIndex; i++) {
      hash = hash * 31 + (uint32_t)wifiDevices[i].rssi;
      hash = hash * 31 + wifiDevices[i].detectionCount;
      hash = hash * 31 + wifiDevices[i].channel;
      hash = hash * 31 + wifiDevices[i].encryptionType;
      for (int j = 0; j < strlen(wifiDevices[i].ssid); j++) {
        hash = hash * 31 + wifiDevices[i].ssid[j];
      }
    }
  } else {
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

  // Actually have data to change, go on
  lastStateHash = currentHash;
  lastDisplayRender = now;

  M5.Display.fillScreen(BLACK);
  drawTopBar();

  // Show scanning message when no devices found yet
  if (deviceIndex == 0 && wifiDeviceIndex == 0) {
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(DARKGREY);
    M5.Display.setCursor(55, 60);
    M5.Display.print("Scanning...");
    xSemaphoreGive(deviceMutex);
    return;
  }

  int y = 15;
  int displayed = 0;
  const int maxDisplay = 3;
  int totalItems = 0;

  if (scanningWiFi) {
    // Allowlisting only applies to BLE devices — never act on a stale BLE
    // address while a WiFi list is on screen.
    topVisibleValid = false;

    totalItems = wifiDeviceIndex;
    for (int i = scrollIndex; i < wifiDeviceIndex && displayed < maxDisplay;
         i++, displayed++) {
      if (displayed > 0) {
        M5.Display.drawFastHLine(0, y - 2, SCREEN_WIDTH, BLUE_GREY);
        y += 1;
      }

      M5.Display.setTextColor(WIFI_NAME_COLOR);
      M5.Display.setTextSize(2);
      M5.Display.setCursor(2, y);
      char ssidDisplay[20];
      if (strlen(wifiDevices[i].ssid) > 0) {
        strncpy(ssidDisplay, wifiDevices[i].ssid, 16);
        ssidDisplay[16] = '\0';
        if (strlen(wifiDevices[i].ssid) > 16) strcat(ssidDisplay, "...");
      } else {
        strcpy(ssidDisplay, "Hidden");
      }
      M5.Display.print(ssidDisplay);
      y += 17;

      M5.Display.setTextSize(1);
      M5.Display.setCursor(2, y);
      M5.Display.setTextColor(YELLOW);
      char mfgDisplay[28];
      getManufacturer(wifiDevices[i].bssid, mfgDisplay, 28);
      M5.Display.print(mfgDisplay);
      y += 9;

      M5.Display.setTextSize(1);
      M5.Display.setCursor(2, y);
      M5.Display.setTextColor(WHITE);
      M5.Display.print("Ch");
      M5.Display.print(wifiDevices[i].channel);
      M5.Display.print(" ");
      M5.Display.setTextColor(GREEN);
      switch (wifiDevices[i].encryptionType) {
        case WIFI_AUTH_OPEN: M5.Display.print("OPEN"); break;
        case WIFI_AUTH_WEP: M5.Display.print("WEP"); break;
        case WIFI_AUTH_WPA_PSK: M5.Display.print("WPA"); break;
        case WIFI_AUTH_WPA2_PSK: M5.Display.print("WPA2"); break;
        case WIFI_AUTH_WPA_WPA2_PSK: M5.Display.print("WPA/2"); break;
        case WIFI_AUTH_WPA2_ENTERPRISE: M5.Display.print("WPA2-E"); break;
        case WIFI_AUTH_WPA3_PSK: M5.Display.print("WPA3"); break;
        case WIFI_AUTH_WPA2_WPA3_PSK: M5.Display.print("WPA2/3"); break;
        case WIFI_AUTH_WAPI_PSK: M5.Display.print("WAPI"); break;
        case WIFI_AUTH_OWE: M5.Display.print("OWE"); break;
        case WIFI_AUTH_WPA3_ENT_192: M5.Display.print("WPA3-E"); break;
        default: M5.Display.print("UNK"); break;
      }
      M5.Display.print(" ");
      M5.Display.setTextColor(DARKGREY);
      M5.Display.print(wifiDevices[i].detectionCount);
      M5.Display.print("x ");
      M5.Display.print(wifiDevices[i].rssi);
      M5.Display.print("dB");
      y += 11;
    }
  } else {
    int filteredCount = 0;
    int sortedIndices[MAX_DEVICES_CAP];

    for (int i = 0; i < deviceIndex; i++) {
      if (filterMode == FILTER_NAMED && strlen(trackedDevices[i].name) == 0) continue;
      if (filterMode == FILTER_ALERTS && !trackedDevices[i].detected) continue;
      sortedIndices[filteredCount++] = i;
    }

    for (int i = 0; i < filteredCount - 1; i++) {
      for (int j = i + 1; j < filteredCount; j++) {
        bool swap = false;
        if (trackedDevices[sortedIndices[i]].detected &&
            trackedDevices[sortedIndices[j]].detected) {
          swap = trackedDevices[sortedIndices[i]].persistenceScore <
                 trackedDevices[sortedIndices[j]].persistenceScore;
        } else if (!trackedDevices[sortedIndices[i]].detected &&
                   trackedDevices[sortedIndices[j]].detected) {
          swap = true;
        } else if (!trackedDevices[sortedIndices[i]].detected &&
                   !trackedDevices[sortedIndices[j]].detected) {
          swap = trackedDevices[sortedIndices[i]].totalCount <
                 trackedDevices[sortedIndices[j]].totalCount;
        }
        if (swap) {
          int temp = sortedIndices[i];
          sortedIndices[i] = sortedIndices[j];
          sortedIndices[j] = temp;
        }
      }
    }

    totalItems = filteredCount;

    if (filteredCount > 0 && scrollIndex < filteredCount) {
      strncpy(topVisibleAddress, trackedDevices[sortedIndices[scrollIndex]].address, 17);
      topVisibleAddress[17] = '\0';
      topVisibleValid = true;
    } else {
      topVisibleValid = false;
    }

    for (int idx = scrollIndex; idx < filteredCount && displayed < maxDisplay;
         idx++, displayed++) {
      int i = sortedIndices[idx];

      if (displayed > 0) {
        M5.Display.drawFastHLine(0, y - 2, SCREEN_WIDTH, BLUE_GREY);
        y += 1;
      }

      if (trackedDevices[i].isSpecial) {
        M5.Display.setTextColor(ORANGE);
      } else if (trackedDevices[i].detected) {
        M5.Display.setTextColor(RED);
      } else {
         M5.Display.setTextColor(BLE_NAME_COLOR);
      }

      M5.Display.setTextSize(2);
      M5.Display.setCursor(2, y);

      char nameDisplay[20];
      if (strlen(trackedDevices[i].name) > 0) {
        strncpy(nameDisplay, trackedDevices[i].name, 16);
        nameDisplay[16] = '\0';
        if (strlen(trackedDevices[i].name) > 16) strcat(nameDisplay, "...");
      } else {
        strcpy(nameDisplay, "Unknown");
      }
      M5.Display.print(nameDisplay);
      y += 17;

      M5.Display.setTextSize(1);
      M5.Display.setCursor(2, y);

      // Show tracker type badge if identified, otherwise manufacturer
      if (trackedDevices[i].trackerType != TRACKER_NONE) {
        M5.Display.setTextColor(RED);
        M5.Display.print("[");
        M5.Display.print(trackerTypeName(trackedDevices[i].trackerType));
        M5.Display.print("] ");
        M5.Display.setTextColor(YELLOW);
        char mfgShort[16];
        strncpy(mfgShort, trackedDevices[i].manufacturer, 15);
        mfgShort[15] = '\0';
        M5.Display.print(mfgShort);
      } else {
        M5.Display.setTextColor(YELLOW);
        char mfgDisplay[28];
        strncpy(mfgDisplay, trackedDevices[i].manufacturer, 27);
        mfgDisplay[27] = '\0';
        M5.Display.print(mfgDisplay);
      }
      y += 9;

      M5.Display.setTextSize(1);
      M5.Display.setCursor(2, y);

      if (trackedDevices[i].detected) {
        M5.Display.setTextColor(RED);
        M5.Display.print("!");
        M5.Display.setTextColor(YELLOW);
        M5.Display.print(trackedDevices[i].persistenceScore, 2);
        char durStr[10];
        unsigned long nowSec = now / 1000;
        unsigned long elapsed = (nowSec >= trackedDevices[i].firstSeen)
                                     ? (nowSec - trackedDevices[i].firstSeen) : 0;
        formatDuration(elapsed, durStr, sizeof(durStr));
        M5.Display.setTextColor(WHITE);
        M5.Display.print(" ");
        M5.Display.print(durStr);
      } else {
        M5.Display.setTextColor(WHITE);
        M5.Display.print(trackedDevices[i].totalCount);
        M5.Display.print("x ");
        M5.Display.print(trackedDevices[i].lastRssi);
        M5.Display.print("dB");
      }

      M5.Display.setCursor(80, y);
      M5.Display.setTextColor(BLUE_GREY);
      M5.Display.print(trackedDevices[i].address);
      y += 11;
    }
  }

  M5.Display.drawFastHLine(0, 133, SCREEN_WIDTH, CYAN);
  M5.Display.drawFastHLine(0, 134, SCREEN_WIDTH, CYAN);

  if (totalItems > 0) {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(YELLOW);

    char countStr[20];
    int showing = std::min(maxDisplay, totalItems - scrollIndex);
    snprintf(countStr, sizeof(countStr), "%d-%d/%d", scrollIndex + 1,
             scrollIndex + showing, totalItems);

    int textWidth = strlen(countStr) * 6;
    int xPos = SCREEN_WIDTH - textWidth - 4;
    M5.Display.setCursor(xPos, 124);
    M5.Display.print(countStr);

    if (paused && totalItems > maxDisplay) {
      M5.Display.setCursor(80, 124);
      M5.Display.setTextColor(GREEN);
      M5.Display.print("A/B:Scroll");
    }
  }

  xSemaphoreGive(deviceMutex);
}

void displayMenuScreen() {
  unsigned long now = millis();

  if (now - lastMenuRender < 500) {
    return;
  }
  lastMenuRender = now;

  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(2, 2);
  M5.Display.setTextColor(GREEN);
  M5.Display.setTextSize(1);
  M5.Display.print("SETTINGS");

  M5.Display.drawLine(0, 12, SCREEN_WIDTH, 12, DARKGREY);

  int y = 16;

  M5.Display.setTextColor(WHITE);
  M5.Display.setCursor(2, y);

  static float lastValidBatVoltage = 3.7f;
  float batVoltage = M5.Power.getBatteryVoltage() / 1000.0f;

  // Filter out obviously bad readings (sensor errors), but allow real low battery
  if (batVoltage > 2.0f && batVoltage < 4.5f) {
    lastValidBatVoltage = batVoltage;
  } else {
    batVoltage = lastValidBatVoltage;
  }

  int batPercent = (int)((batVoltage - 3.0f) / 1.2f * 100.0f);
  if (batPercent > 100) batPercent = 100;
  if (batPercent < 0) batPercent = 0;
  float estHoursRemaining = (batPercent / 100.0f) * TYPICAL_BATTERY_LIFE_HOURS;
  M5.Display.print("Bat:");
  M5.Display.print(batPercent);
  M5.Display.print("% (~");
  if (estHoursRemaining >= 1.0f) {
    M5.Display.print(estHoursRemaining, 1);
    M5.Display.print("h)");
  } else {
    M5.Display.print((int)(estHoursRemaining * 60));
    M5.Display.print("m)");
  }
  M5.Display.print(" Br:");
  M5.Display.print(highBrightness ? "Hi" : "Lo");
  y += 12;

  // Compact: Timeout/RAM/Tracked share one line so 6 menu rows still fit
  // this screen's 135px height.
  M5.Display.setCursor(2, y);
  M5.Display.print("T:");
  M5.Display.print(screenTimeoutMs / 1000);
  M5.Display.print("s RAM:");
  M5.Display.print(ESP.getFreeHeap() / 1024);
  M5.Display.print("K Trk:");
  M5.Display.print(deviceIndex);
  y += 16;

  M5.Display.drawLine(0, y, SCREEN_WIDTH, y, DARKGREY);
  y += 4;

  menuBaseY = y;

  M5.Display.setTextColor(CYAN);

  M5.Display.setCursor(10, y);
  M5.Display.print("Toggle Brightness");
  y += 11;

  M5.Display.setCursor(10, y);
  M5.Display.print("Set Screen Timeout");
  y += 11;

  M5.Display.setCursor(10, y);
  M5.Display.print("Alert Mode");
  y += 11;

  M5.Display.setCursor(10, y);
  M5.Display.print("Export Incident");
  y += 11;

  M5.Display.setCursor(10, y);
  M5.Display.print("Clear Devices");
  y += 11;

  M5.Display.setCursor(10, y);
  M5.Display.print("Shutdown");
  y += 12;

  M5.Display.drawLine(0, y, SCREEN_WIDTH, y, DARKGREY);

  M5.Display.setTextColor(YELLOW);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(2, y + 3);
  M5.Display.print("A:Nav B:Sel A+B:Exit");
}

void highlightMenuOption(int index) {
  for (int i = 0; i < MENU_OPTION_COUNT; i++) {
    M5.Display.setCursor(2, menuBaseY + (i * 11));
    M5.Display.setTextColor(BLACK);
    M5.Display.print(">");
  }

  M5.Display.setCursor(2, menuBaseY + (index * 11));
  M5.Display.setTextColor(YELLOW);
  M5.Display.print(">");
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
  file.print("discreet=");
  file.println(discreetAlerts ? "1" : "0");
  file.print("sound=");
  file.println(alertSoundEnabled ? "1" : "0");
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

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();

    if (line.startsWith("brightness=")) {
      highBrightness = line.substring(11).toInt() == 1;
    } else if (line.startsWith("timeout=")) {
      screenTimeoutMs = line.substring(8).toInt();
    } else if (line.startsWith("discreet=")) {
      discreetAlerts = line.substring(9).toInt() == 1;
    } else if (line.startsWith("sound=")) {
      alertSoundEnabled = line.substring(6).toInt() == 1;
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
    if (strcmp(runtimeAllowlist[i], address) == 0) {
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
  scrollIndex = 0;

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
      toggleBrightness();
      showFeedback(highBrightness ? "BRIGHT" : "DIM", CYAN);
      delay(1000);
      break;
    case 1:
      cycleScreenTimeout();
      break;
    case 2:
      cycleAlertMode();
      break;
    case 3: {
      int exported = exportIncident();
      char msg[16];
      snprintf(msg, sizeof(msg), "%d EXPORTED", exported);
      showFeedback(msg, exported > 0 ? GREEN : DARKGREY);
      delay(1000);
      break;
    }
    case 4:
      clearDevices();
      showFeedback("CLEARED", GREEN);
      delay(1000);
      break;
    case 5:
      shutdownDevice();
      return;
  }
  
  forceDisplayRefresh();
  displayMenuScreen();
  highlightMenuOption(menuIndex);
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

// 4-state cycle: Loud+Sound -> Loud+Mute -> Quiet+Sound -> Quiet+Mute -> repeat.
// Combined into one setting (rather than two independent toggles) to save a
// menu row on a screen that's already tight on vertical space.
void cycleAlertMode() {
  if (!discreetAlerts && alertSoundEnabled) {
    alertSoundEnabled = false;
  } else if (!discreetAlerts && !alertSoundEnabled) {
    discreetAlerts = true;
    alertSoundEnabled = true;
  } else if (discreetAlerts && alertSoundEnabled) {
    alertSoundEnabled = false;
  } else {
    discreetAlerts = false;
    alertSoundEnabled = true;
  }
  saveUserPreferences();

  const char *label = discreetAlerts
                           ? (alertSoundEnabled ? "QUIET+SOUND" : "QUIET+MUTE")
                           : (alertSoundEnabled ? "LOUD+SOUND" : "LOUD+MUTE");
  showFeedback(label, discreetAlerts ? CYAN : ORANGE);
  delay(1000);
}

bool checkButtonCombo() {
  unsigned long currentMillis = millis();

  if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
    if (currentMillis - lastComboCheck > 300) {
      lastComboCheck = currentMillis;
      return true;
    }
  }
  return false;
}

void handleBtnA() {
  lastButtonPressTime = millis();
  lastActivityTime = millis();

  if (paused) {
    unsigned long pressStart = millis();

    while (M5.BtnA.isPressed() && (millis() - pressStart < 1000)) {
      M5.update();
      delay(10);
    }

    if (millis() - pressStart >= 1000) {
      // Long-press: allowlist the topmost visible device — kills a false
      // positive on the spot, no reflash needed. BLE-only (see topVisibleValid).
      if (topVisibleValid) {
        char allowedAddr[18];
        strncpy(allowedAddr, topVisibleAddress, 17);
        allowedAddr[17] = '\0';
        if (allowlistDevice(allowedAddr)) {
          showFeedback("ALLOWLISTED", GREEN, allowedAddr);
        } else {
          showFeedback("ALLOW FULL", RED);
        }
      } else {
        showFeedback("NOTHING", DARKGREY);
      }
      delay(1200);
      lastStateHash = 0;
      lastDisplayRender = 0;
      displayTrackedDevices();
    } else if (scrollIndex > 0) {
      scrollIndex--;
      lastStateHash = 0;
      lastDisplayRender = 0;
      displayTrackedDevices();
    }
  } else {
    paused = true;
    scrollIndex = 0;
    showFeedback("PAUSED", RED, "Hold B to Resume");
    delay(1500);
    lastStateHash = 0;
    lastDisplayRender = 0;
    displayTrackedDevices();
  }
}

void handleBtnB() {
  lastButtonPressTime = millis();
  lastActivityTime = millis();

  if (paused) {
    unsigned long pressStart = millis();

    while (M5.BtnB.isPressed() && (millis() - pressStart < 1000)) {
      M5.update();
      delay(10);
    }

    if (millis() - pressStart >= 1000) {
      paused = false;
      scrollIndex = 0;
      showFeedback("RESUMED", GREEN);
      delay(800);
      lastStateHash = 0;
      lastDisplayRender = 0;
      displayTrackedDevices();
    } else {
      int maxScroll = scanningWiFi ? wifiDeviceIndex - 3 : deviceIndex - 3;
      if (maxScroll < 0) maxScroll = 0;
      if (scrollIndex < maxScroll) {
        scrollIndex++;
        lastStateHash = 0;
        lastDisplayRender = 0;
        displayTrackedDevices();
      }
    }
  } else {
    filterMode = (FilterMode)((filterMode + 1) % 3);
    const char *label = filterMode == FILTER_NAMED ? "NAMED ONLY"
                       : filterMode == FILTER_ALERTS ? "ALERTS ONLY"
                       : "SHOW ALL";
    uint16_t color = filterMode == FILTER_NAMED ? CYAN
                    : filterMode == FILTER_ALERTS ? RED
                    : ORANGE;
    showFeedback(label, color);
    delay(800);
    lastStateHash = 0;
    lastDisplayRender = 0;
    displayTrackedDevices();
  }
}

void forceDisplayRefresh() {
  lastStateHash = 0;
  lastDisplayRender = 0;
  lastMenuRender = 0;
}

void handleButtonCombination() {
  inMenu = !inMenu;
  lastActivityTime = millis();
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

    if (localScanningWiFi) {
      // WiFi Scan (blocking)
      int n = WiFi.scanNetworks(false, false, false, 300);

      if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        for (int i = 0; i < n; i++) {
          trackWiFiDevice(WiFi.SSID(i).c_str(), WiFi.BSSIDstr(i).c_str(),
                          WiFi.RSSI(i), WiFi.channel(i), WiFi.encryptionType(i),
                          currentTime);
          vTaskDelay(1 / portTICK_PERIOD_MS);
        }
        xSemaphoreGive(deviceMutex);
      }
      WiFi.scanDelete();

    } else {
      // BLE Scan (blocking)
      NimBLEScanResults foundDevices = pBLEScan->getResults(2000, false);

      {
        // A single scan batch can surface several distinct new trackers at
        // once (e.g. walking past a shelf of AirTags). Queue every one of
        // them here instead of only ever showing the last — trackedDevices[0]
        // is only *this* device right when trackDevice() returns true for it
        // (it just moved itself there), so snapshot immediately or a later
        // device in the same batch overwrites it and the alert is lost.
        PendingAlertInfo alertQueue[8];
        int queuedAlerts = 0;

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
                queuedAlerts++;
              }
            }
            vTaskDelay(1 / portTICK_PERIOD_MS);
          }
          xSemaphoreGive(deviceMutex);
        }

        // Show every queued alert in turn, outside the mutex.
        for (int q = 0; q < queuedAlerts; q++) {
          pendingAlertInfo = alertQueue[q];
          showingAlert = true;
          alertPending = true;

          // Hold here until loop() (Core 1) has shown the alert and the user
          // dismissed it. Keep feeding the watchdog — this wait is expected
          // to last as long as the user takes to notice the alert.
          while (showingAlert && scanTaskRunning) {
            esp_task_wdt_reset();
            vTaskDelay(50 / portTICK_PERIOD_MS);
          }
        }

        pBLEScan->clearResults();
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
  Serial.println("Starting setup...");

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
  Serial.println("Type 'help' over serial for the no-reflash config console.");

  Serial.print("Screen Timeout: ");
  Serial.println(screenTimeoutMs);

  deviceMutex = xSemaphoreCreateMutex();
  if (deviceMutex == NULL) {
    Serial.println("ERROR: Failed to create device mutex!");
    while (1) { delay(300); }
  }
  Serial.println("Device mutex created");
  
  delay(1000);

  M5.Display.fillScreen(BLACK);
  drawTopBar();

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(DARKGREY);
  M5.Display.setCursor(60, 60);
  M5.Display.print("Starting scans...");
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
    alertUser(local.isSpecial, local.name, local.mac, local.score, local.trackerType);
    showingAlert = false;
    forceDisplayRefresh();
    lastDisplayUpdate = currentMillis;
    return;
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
      highlightMenuOption(menuIndex);
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

  // MENU COMBO CHECK (both buttons)
  if (screenOn && checkButtonCombo()) {
    inMenu = !inMenu;
    lastActivityTime = currentMillis;
    lastDisplayRender = 0;
    lastMenuRender = 0;
    lastStateHash = 0;

    if (inMenu) {
      menuIndex = 0;
      displayMenuScreen();
      highlightMenuOption(menuIndex);
    } else {
      displayTrackedDevices();
    }
    lastDisplayUpdate = currentMillis;
    return;
  }

  // HANDLE INPUT BASED ON MODE
  if (screenOn) {
    if (inMenu) {
      // MENU MODE
      if (btnA && (currentMillis - lastBtnAPress > DEBOUNCE_DELAY)) {
        lastBtnAPress = currentMillis;
        menuIndex = (menuIndex + 1) % MENU_OPTION_COUNT;
        highlightMenuOption(menuIndex);
        return;
      }
      if (btnB && (currentMillis - lastBtnBPress > DEBOUNCE_DELAY)) {
        lastBtnBPress = currentMillis;
        executeMenuOption(menuIndex);
        return;
      }
    } else {
      // NORMAL MODE
      if (btnA && (currentMillis - lastBtnAPress > DEBOUNCE_DELAY)) {
        lastBtnAPress = currentMillis;
        handleBtnA();
        return;
      }
      if (btnB && (currentMillis - lastBtnBPress > DEBOUNCE_DELAY)) {
        lastBtnBPress = currentMillis;
        handleBtnB();
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
      highlightMenuOption(menuIndex);
    } else {
      displayTrackedDevices();
    }
    lastDisplayUpdate = currentMillis;
  }

  vTaskDelay(50 / portTICK_PERIOD_MS);
}
