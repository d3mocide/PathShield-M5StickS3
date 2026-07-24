# PathShield M5StickS3 — UX & Product Roadmap

Enhancements identified during the S3 stability pass, scoped into three phases by
impact. Sequenced low → medium → high: quick, low-risk wins first to build on the
stability fixes, saving the larger architectural/product swings for last.

Each item lists status, what it is, and why it matters. Check items off as they land;
keep the "why" text even after completion so this stays a useful record, not just a
checklist.

## Phase 1 — Low Impact (quick, additive, low risk)

- [x] **Battery time-remaining estimate.** Settings menu's battery line now shows
  `Bat:72% (~3.2h) Br:Hi` instead of just the percentage — a rough linear estimate
  against the README's documented 4-6h continuous-scan range
  (`TYPICAL_BATTERY_LIFE_HOURS`), not real current-draw telemetry. Helps a user mid-session
  decide whether to top up.
- [x] **Duration since first seen.** Alerting/detected devices in the findings list now
  show elapsed time next to the persistence score (e.g. `!0.82 3m`, `!0.91 1h05m`).
  "Here for over an hour" is a more intuitive read than a raw 0-1 score. Scoped to
  detected devices only — the row has room there; background/undetected rows keep the
  existing count+RSSI format since screen space is tight at 240x135.
- [x] **Alerts-only filter.** The existing Button B filter (previously a 2-state
  Show All / Named Only toggle) is now a 3-state cycle: All → Named Only → Alerts
  Only → All. Reuses the existing gesture, no new UI surface. Lets a user jump
  straight to what's currently flagged instead of paging through every background
  BLE/WiFi device.

## Phase 2 — Medium Impact (new interaction/hardware surface, contained scope)

- [x] **Discreet alert mode + speaker tone.** Combined into one 4-state "Alert
  Mode" menu cycle (Loud+Sound → Loud+Mute → Quiet+Sound → Quiet+Mute) rather
  than two independent toggles, to save a menu row on a screen that was
  already nearly full at 4 options. Quiet mode replaces the 5x full-screen
  red/blue strobe with a black screen + thin colored border, and no longer
  forces max brightness (respects whatever brightness the user already
  chose) — the point is to not draw attention. Sound uses the StickS3's
  ES8311 speaker (`M5.Speaker.tone()`, previously unused) for a short
  double-beep, independent of the visual mode. Persisted to `/prefs.txt`.
- [x] **Flag/allowlist a device from the paused findings list.** Shipped the
  allowlist half of this item: hold Button A (1s) on the paused BLE list to
  allowlist the topmost visible device — exact-MAC match (not an OUI prefix
  like the compile-time `allowlistMacs[]`, to avoid a quick in-field action
  accidentally suppressing a different device from the same manufacturer),
  removes it from the tracked list immediately, persists to
  `/allowlist.txt`. **Not shipped:** "flag as special/watch more closely" —
  no clean second long-press gesture was available without overloading
  Button B's existing hold-to-resume, and allowlisting was the more
  directly valuable half (it's the one the README's troubleshooting section
  already flagged as a pain point). Could revisit via the settings menu if
  it's still wanted.
- [x] **On-demand "Export Incident" menu action.** Deliberate, user-triggered
  snapshot of currently-alerting devices (MAC, manufacturer, tracker type,
  score, first-seen uptime) appended to `/incidents.txt` on SPIFFS —
  distinct from the passive `/devices.txt` logging removed earlier.
  Timestamps are uptime-relative (no RTC/NTP on this device), not
  wall-clock. **Retrieval:** no WiFi/USB export path exists yet (that's
  Phase 3 territory), so for now the only way to get an export off the
  device is Serial Monitor (115200 baud) + send `d` to dump the file. Good
  enough to make the feature actually usable today; a proper retrieval path
  will likely piggyback on Phase 3's no-reflash-configuration work.

## Phase 3 — High Impact (larger scope, more design/validation risk)

- [x] **No-reflash configuration.** Shipped as a line-based serial command
  console (`help`, `special list/add/remove/reset`, `allow list/add/remove`,
  `threshold list/set/reset`, `dump`, `config`) over the existing USB-serial
  connection, persisted to SPIFFS immediately. Chose serial over a temporary
  WiFi-AP config page: this is an anti-stalking device whose Quiet mode
  exists specifically to avoid drawing attention, so having it broadcast a
  discoverable WiFi AP for configuration would cut against its own purpose —
  serial requires physical USB access and stays silent on RF. Widened scope
  slightly beyond just "allowlist/specialMacs/thresholds": also added
  allowlist *removal* (`allow remove`), which didn't exist in any form
  before — the Button-A-hold gesture could only add.
- [ ] **IMU-based motion correlation.** The StickS3's BMI270 6-axis IMU is
  currently unused. Correlating tracker RSSI drift against the wearer's actual
  motion (rather than RSSI variance alone, today's rough proxy) could meaningfully
  cut false positives. Algorithm-level change — needs real-world tuning and
  validation before it should ship, not a quick add.

## Already completed (context, not part of this roadmap's phases)

The stability pass that preceded this roadmap: fixed the Core0/Core1 display-and-button
race condition causing freezes and unresponsive buttons, re-armed the task watchdog,
fixed a bug where simultaneous new-tracker detections in one scan batch could silently
drop all but the last alert, dropped M5StickC Plus 1.1/Plus 2 support to focus this fork
on the S3 exclusively, and removed the dead `/devices.txt` write-only persistence. See
PR history for detail.
