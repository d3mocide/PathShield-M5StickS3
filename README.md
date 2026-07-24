<div align="center">
  
  [![Pre-release](https://img.shields.io/github/v/release/lukeswitz/PathShield?include_prereleases&label=pre-release&color=orange)](https://github.com/lukeswitz/PathShield/releases)
[![GitHub code size in bytes](https://img.shields.io/github/languages/code-size/lukeswitz/PathShield)](https://github.com/lukeswitz/AntiHunter/tree/main/PathShield/)
  
<img height="500" alt="image" src="https://github.com/user-attachments/assets/0dac6a9f-32a8-4b05-b6a1-b59fc3762f51" />

PathShield is an RF awareness tool for the M5StickS3. It uses BLE/WiFi scanning to detect nearby devices, alerting on those following you.
</div>


---

> [!NOTE]
> This fork targets the **M5StickS3 exclusively** — the code, web flasher, and docs here assume that hardware and take on breaking changes freely. For M5StickC Plus 1.1 / Plus 2, use the upstream project: [lukeswitz/PathShield](https://github.com/lukeswitz/PathShield).

> [!CAUTION]
> **ETHICAL USE ONLY**
> Users are solely responsible for: Compliance with all applicable laws. Respecting reasonable expectations of privacy


## Table of Contents

1. [Features](#features)
2. [Installation](#installation)
3. [Controls](#controls)
4. [Display Guide](#display-guide)
5. [Detection Algorithm](#detection-algorithm)
6. [No-Reflash Configuration](#no-reflash-configuration)
7. [Customization](#customization)
8. [Troubleshooting](#troubleshooting)
9. [Known Limitations](#known-limitations)
10. [Credits](#credits)
11. [License](#license)


## Features

- **Dual-Band Scanning**: Alternates WiFi and BLE detection, displays MAC, vendor, SSID, channel, hit count & RSSI
- **Persistence Scoring**: Multi-factor algorithm reduces false positives. Allowlist for known devices
- **Real-Time Alerts**: Visual notifications for detected trackers and user-specified MAC targets
- **Tracker Detection**: AirTag, Tile, SmartTag, Chipolo, Google FMDN identified by protocol
- **Known Device Ring Buffer**: Stable-RSSI devices promoted to compact storage, freeing active slots
- **24,500+ MAC Database**: Offline manufacturer identification
- **PSRAM-Backed Tracking**: Uses the StickS3's 8MB PSRAM for a large device-tracking capacity

![image](https://github.com/user-attachments/assets/fade8692-0052-4e00-b244-b068992c8772)


> [!TIP]
> Configure the allowlist, target MAC prefixes, and sensitivity thresholds over USB serial — no reflash needed. See [No-Reflash Configuration](#no-reflash-configuration).

## Installation

### Web Flasher
[Install PathShield](https://d3mocide.github.io/pathshield-m5sticks3/)

1. Open link in Chrome, Edge, or Opera (not Safari/Firefox)
2. Connect M5StickS3 via USB-C
3. Click "Deploy Firmware"
4. Select serial port, wait ~2 minutes

### From Source (Arduino IDE)

**M5StickS3:**
1. Board package: `m5stack:esp32` (v3.3.8+), board **M5StickS3**
2. PSRAM: **OPI PSRAM** (default) — required, the firmware halts on boot if it isn't enabled
3. Partition: **8M with spiffs (3MB APP/1.5MB SPIFFS)** (default)

**Library versions:**
- **M5Unified**: `>= 0.2.14` required, `0.2.17+` recommended — earlier versions have a StickS3-specific bug where `M5.Power.powerOff()` powers off and then immediately reboots instead of staying off
- **M5GFX**: latest (pulled in automatically as an M5Unified dependency)
- **NimBLE-Arduino**: latest


## Controls

### Normal Scanning Mode
```
Button A:   Pause scanning
Button B:   Cycle filter (Show All -> Named Only -> Alerts Only)
A+B (hold): Settings menu
```

### Paused 
```
Button A:      Scroll up (tap)
Button A hold: Allowlist the topmost visible device (hold 1 second)
Button B:      Scroll down (tap)
Button B hold: Resume (hold 1 second)
```

Holding Button A allowlists whichever device is currently at the top of the
screen — it disappears from the list immediately and won't be tracked again.
Useful for killing a false positive (your own phone, earbuds, car) on the
spot, without editing `allowlistMacs[]` and reflashing. Only works on BLE
devices (not the WiFi list), and matches the exact MAC shown, not the whole
manufacturer. The allowlist persists across reboots (`/allowlist.txt`). To
remove an entry later, or to allowlist a MAC you already know without
waiting for it to show up on screen, use the `allow` serial command — see
[No-Reflash Configuration](#no-reflash-configuration).

### Settings Menu
```
Button A:  Navigate options (up/down)
Button B:  Select option
A+B hold:  Exit menu
```

**Available Settings:**
- **Toggle Brightness**: Low/High (saves battery on low brightness)
- **Set Screen Timeout**: How long before screen turns off when idle (10-300 seconds)
- **Alert Mode**: Cycles Loud+Sound / Loud+Mute / Quiet+Sound / Quiet+Mute. Quiet mode skips the full-screen red/blue strobe (a small bordered indicator instead) and doesn't force max brightness — useful when a flashing screen would draw the wrong kind of attention. Sound plays a short double-beep on alert via the StickS3's onboard speaker.
- **Export Incident**: Writes a timestamped snapshot of currently-alerting devices to `/incidents.txt` on SPIFFS. Deliberate/on-demand only — nothing is logged automatically. Retrieve it by opening Serial Monitor (115200 baud, newline line ending) and sending `dump`.
- **Clear Devices**: Clears all tracked devices from memory
- **Shutdown**: Power off the device

## Display Guide

### Top Status Bar
- **Scan Mode**: `SCAN` (green = actively scanning), `PAUSE` (red = paused)
- **WiFi/BLE Indicator**: Current scan mode (WiFi or Bluetooth)
- **Battery Bar**: Device battery percentage (0-100%)
- **Memory Bar**: Available RAM in KB (green = good, yellow = warning, red = critical)

### Device List
Each device shows:
```
Device Name (BLE) or SSID (WiFi)
Manufacturer (identified from MAC)
Detection Count + Signal Strength (RSSI)
  -- or, once flagged as a suspected tracker --
Alert Score + Duration Since First Seen (e.g. "!0.82 3m")
```

### Color Codes
```
CYAN    = WiFi networks / Normal Bluetooth devices
ORANGE  = User-defined tracker (special MAC)
RED     = Suspected tracker detected (high persistence score)
YELLOW  = Manufacturer name
GREEN   = Scan active, status messages
```

### Filter Mode
- Press **Button B** to cycle: "Show All" -> "Named Only" (hides unnamed/noise devices) -> "Alerts Only" (only currently-flagged/suspected trackers) -> back to "Show All"
- Useful for cutting through noise when there are many unnamed devices, or jumping straight to what's currently flagged

### Footer
- **Page counter**: Shows which page you're viewing (e.g., "1-3/23")
- **Scroll hint**: When paused, shows navigation instructions

## Detection Algorithm

### How Tracker Identification Works

PathShield uses a **multi-layer packet inspection approach** to reliably identify trackers from BLE advertisement data. Since modern trackers use standardized Bluetooth identifiers, this method catches them regardless of MAC randomization.

**Layer 1: Manufacturer Data (Company IDs)**

BLE packets include manufacturer-specific data identified by a 16-bit company ID (stored little-endian). Known trackers advertise predictable company IDs:

- **AirTag & Find My Devices**: Company ID `0x004C` (Apple Inc.)
  - Must have ≥27 bytes of payload
  - Payload type byte at offset 2 must be `0x12` or `0x07` (Find My authentication)
  - Also catches: Chipolo ONE Spot, Eufy cameras in Find My mode

- **Samsung SmartTag**: Company ID `0x0075` (Samsung Electronics)

- **Chipolo (native mode)**: Company ID `0x0133` (Chipolo d.o.o.)

- **Google FMDN**: Company ID `0x00E0` (Google Find My Device Network)

**Layer 2: Service UUIDs**

Some trackers advertise standard BLE service UUIDs instead of (or in addition to) manufacturer data:

- **Tile**: Service UUIDs `0xFD51` or `0xFD52`
- **Samsung SmartTag (alt)**: Service UUID `0xFD6F` (SmartThings/Find)

**Layer 3: Device Name (Fallback)**

If manufacturer data and service UUIDs don't match, PathShield falls back to case-insensitive name matching:
- Device name contains "tile" → Tile tracker
- Device name contains "smarttag" → Samsung SmartTag
- Device name contains "chipolo" → Chipolo

This layered approach works because trackers must advertise these identifiers to function — they can't hide their identity without breaking their intended purpose. The combination of multiple detection methods eliminates false negatives.

### Persistence Scoring (0.0 - 1.0)

**Factor 1: Detection Frequency (0.25 max)**
- Requires minimum 8 detections
- More frequent = higher score

**Factor 2: Time Window Distribution (0.30 max)**
- Tracks across 4 windows: 5/10/15/20 minutes
- Must appear in 3+ windows
- Consistent presence = tracking behavior

**Factor 3: ε-Connectedness (0.25 max)**
- No gaps >3 minutes between detections
- Continuous tracking pattern
- Distinguishes stalking from coincidence

**Factor 4: RSSI Pattern (0.20 max)**
- Signal strength variations
- Movement correlation

**Alert Threshold: ≥ 0.75 by default** — configurable without reflashing, see below.



## No-Reflash Configuration

Allowlist entries, target MAC prefixes, and sensitivity thresholds can all be
changed over USB serial while the device is running — no Arduino IDE, no
reflash. This is deliberately serial-only rather than a WiFi config page:
PathShield is an anti-stalking device (Quiet mode exists specifically so it
doesn't draw attention), so it shouldn't itself broadcast a discoverable
WiFi access point. Serial requires physical USB access and stays silent on
RF.

**Connect:** Open a Serial Monitor at 115200 baud with the line ending set
to **Newline** (or **Both NL & CR**) — commands are dispatched on Enter, not
per keystroke. Type `help` and press Enter for the full command list.

```
help                             Show the full command list
dump                             Print /incidents.txt
config                           Show full current configuration
special list                     List privacy-invader MAC prefixes
special add <prefix>             e.g. special add 00:25:DF
special remove <index|prefix>    Remove by index (from 'special list') or exact text
special reset                    Reset to compiled-in defaults
allow list                       List runtime allowlist (exact MACs)
allow add <MAC>                  e.g. allow add AA:BB:CC:DD:EE:FF
allow remove <MAC>               Remove a MAC from the allowlist
threshold list                   Show sensitivity thresholds
threshold set <name> <value>     name: persistence | rssi_stability | rssi_variation
threshold reset                  Reset thresholds to defaults
```

- `special` manages the same OUI-prefix list as the compiled-in `specialMacs[]`
  (default: Axon camera / Flock Safety OUIs) — anything matching triggers an
  immediate "KNOWN" alert. Prefixes only, e.g. `00:25:DF`, not a full MAC.
- `allow` manages the same runtime allowlist the Button-A-hold gesture writes
  to (`/allowlist.txt`), but with a remove path and the ability to add a MAC
  you already know without waiting for it to appear on screen. Requires the
  full 17-character MAC (`AA:BB:CC:DD:EE:FF`) for an exact match.
- `threshold set persistence <0.0-1.0>` raises or lowers the alert bar
  directly (default `0.75`, see [Detection Algorithm](#detection-algorithm)).
  `rssi_stability`/`rssi_variation` (default `10`/`15` dBm) tune how RSSI
  swings feed into that score — see [Adjust Sensitivity](#adjust-sensitivity)
  for what raising/lowering each one trades off.

All changes are written to SPIFFS immediately and persist across reboots —
equivalent to editing `PathShield.ino` and reflashing, without the toolchain.


## Customization

> [!NOTE]
> Everything in this section can also be changed live over serial without
> touching source or reflashing — see [No-Reflash Configuration](#no-reflash-configuration).
> Editing the values here instead changes the *compiled-in default* a fresh
> install (or a `special reset` / `threshold reset`) falls back to.

### Known Tracker MACs

Edit `defaultSpecialMacs[]` in PathShield.ino — this seeds the runtime list on
first boot and whenever `special reset` is used:
```cpp
const char *defaultSpecialMacs[] = {
  // Apple OUIs (AirTags use rotating addresses from Apple's OUI space)
  "AC:DE:48",  // Apple Inc.
  "F0:98:9D",  // Apple Inc.
  "BC:92:6B",  // Apple Inc.
  
  // Tile
  "C4:AC:05",  // Tile Inc.
  "E0:00:00",  // Tile Inc. (some models)
  
  // Samsung SmartTag
  "E4:5F:01",  // Samsung Electronics
  "74:5C:4B",  // Samsung Electronics
  "E8:50:8B",  // Samsung Electronics (additional)
  
  // Chipolo
  "EC:81:93",  // Chipolo d.o.o.
};
```

### Allowlist (Trusted Devices)

For most cases, use the `allow add <MAC>` serial command or the in-field
Button-A-hold gesture instead — both allowlist by exact MAC and need no
reflash. `allowlistMacs[]` in PathShield.ino is a separate, OUI-*prefix*
compiled-in allowlist for when you want to ignore every device from a given
manufacturer, not just one:

```cpp
const char *allowlistMacs[] = {
  "AA:BB:CC",  // Your trusted device 1
  "DD:EE:FF",  // Your trusted device 2
  // Add MAC prefixes of devices you own
};
```

Allowlisted devices are completely ignored during scanning and will never trigger tracker alerts.

### Adjust Sensitivity

The persistence threshold and RSSI thresholds below are runtime-configurable —
use `threshold set <name> <value>` (see [No-Reflash Configuration](#no-reflash-configuration))
instead of editing source for these two. `MIN_DETECTIONS` and `MIN_WINDOWS`
are compile-time only (they change the shape of the scoring algorithm itself,
not just where its output gets gated).

**More Sensitive (more alerts):**
```cpp
threshold set persistence 0.50   // Lower threshold
```
```cpp
#define MIN_DETECTIONS 5            // Fewer detections needed
#define MIN_WINDOWS 2               // Fewer time windows
```

**Less Sensitive (fewer false positives):**
```cpp
threshold set persistence 0.75   // Higher threshold (default)
```
```cpp
#define MIN_DETECTIONS 12           // More detections needed
#define MIN_WINDOWS 4               // More time windows
```

**Scan Timing:**
```cpp
#define SCAN_SWITCH_INTERVAL 3000   // 3 seconds each (WiFi/BLE)
// Change to 2000 for faster scanning
// Change to 5000 for slower, battery-saving
```

### Display Customization

**Max devices shown:**
```cpp
const int maxDisplay = 3;  // Change to 2 or 4
```

**Color scheme (in displayTrackedDevices):**
```cpp
M5.Display.setTextColor(CYAN);     // Change to GREEN, BLUE, etc.
M5.Display.drawFastHLine(0, 0, SCREEN_WIDTH, MAGENTA);  // Border color
```

## Troubleshooting

### No Alerts for Known Tracker

**Solution:**
1. Lower threshold temporarily over serial (no reflash needed):
```
threshold set persistence 0.40
```
2. Check Serial Monitor (115200 baud) for detection counts
3. Verify tracker is powered on and advertising
4. `threshold reset` to go back to the default afterward

### Too Many False Positives

**Quick fix: Use name filter**
- Press **Button B** to show only named devices
- Hides random MAC addresses and noise

**Fastest fix: Allowlist it in the field**
1. Press **Button A** to pause, scroll until the device is at the top
2. Hold **Button A** for 1 second — it's gone and won't be tracked again
3. No reflash needed; persists across reboots

**Also works from a shell, no button gesture needed**
```
allow add AA:BB:CC:DD:EE:FF
```
Made a mistake, or want it back? `allow remove AA:BB:CC:DD:EE:FF`.

**Permanent, compiled-in default (for devices you always want ignored, even after a fresh flash)**
1. Note the MAC address from the display
2. Add to `allowlistMacs[]` in PathShield.ino
3. Re-upload and restart

**Fine-tune sensitivity**
```
threshold set persistence 0.85       // Raise to be more strict
threshold set rssi_stability 6       // Require tighter RSSI stability
```
(`MIN_DETECTIONS` stays compile-time-only — see [Adjust Sensitivity](#adjust-sensitivity).)

### Cannot Resume from Pause

Hold Button B for 1 full second (not just tap).

### Device Crashes / Resets

Watch the memory bar on screen — red means critically low. If a BLE or WiFi scan genuinely hangs, a task watchdog reboots the device automatically after ~20 seconds rather than leaving it frozen.

### SPIFFS Format on First Boot

Normal on first flash. The device formats SPIFFS automatically (~30 seconds), then boots normally.

### Button Not Responding

- Screen may be dimmed (press any button to wake)
- Wait 200ms between presses (debounce)
- For menu: hold both buttons 300ms+


## Hardware

| | M5StickS3 |
|---|---|
| **SoC** | ESP32-S3-PICO-1-N8R8 |
| **Flash** | 8MB |
| **PSRAM** | 8MB (Octal) |
| **Device Limits** | ~70 BLE, ~50 WiFi |

> [!NOTE]
> Device limits are fixed at boot, sized for the StickS3's 8MB PSRAM. That's well above what's currently used, so the caps in `PathShield.ino` (`MAX_DEVICES_CAP`, `MAX_WIFI_DEVICES_CAP`, `MAX_KNOWN_CAP`) can be raised in a future update once headroom is confirmed on hardware.

## Known Limitations

1. **MAC Randomization**: Modern phones randomize MACs — use name filter
2. **Range Limited**: BLE/WiFi ranges vary by environment
3. **No GPS**: Detects proximity only, not location
4. **Battery**: Continuous dual-band scanning drains battery in 4-6 hours

## Credits

Detection algorithms based on:
- [Chasing-Your-Tail-NG](https://github.com/ArgeliusLabs/Chasing-Your-Tail-NG) - Persistence tracking
- [BLE-Doubt](https://arxiv.org/abs/2205.12235) - Topological classification
- [CreepDetector](https://github.com/AlexLynd/CreepDetector) - Original concept

## License

MIT License - Use at your own risk. Developers provide no warranty and accept no liability for unlawful or
unethical use. Review local regulations before deployment.

## Contributing

Issues and pull requests welcome. Test thoroughly before submitting.

See [ROADMAP.md](ROADMAP.md) for planned UX/product enhancements, phased by impact.

## Support

- GitHub Issues: Bug reports and feature requests
- Serial Monitor: Enable debugging (115200 baud)
- [Web Flasher](https://lukeswitz.github.io/PathShield/)
