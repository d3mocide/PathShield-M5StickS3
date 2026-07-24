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

- [ ] **Discreet alert mode.** A menu toggle to replace the current 5x full-screen
  red/blue strobe with a quiet variant (small persistent indicator, no strobe) for
  situations where a flashing screen is the opposite of what you want — this is a
  personal-safety tool, and drawing attention to the fact you're checking for a
  tracker can itself be a risk.
- [ ] **Speaker tone on alert.** The StickS3 has an ES8311 codec + speaker that this
  firmware never touches (`M5.Speaker` is unused). Add a short configurable tone
  (on/off, maybe volume) as an alert channel independent of the screen — useful
  alongside or instead of discreet mode.
- [ ] **Flag/allowlist a device from the paused findings list.** Currently the only
  way to add a device to `specialMacs[]`/`allowlistMacs[]` is editing the source and
  reflashing (the README's own troubleshooting section says as much). A long-press
  on the selected device while paused to allowlist it (kill a false positive) or
  flag it (watch more closely) removes a real friction point — the single biggest
  gap between "tool you compile" and "tool a non-engineer can use in the field."
- [ ] **On-demand "Export Incident" menu action.** Distinct from the passive
  `/devices.txt` logging we removed (write-only, never read, no product value) —
  this is a deliberate, user-triggered snapshot of currently-alerting devices
  (MAC, manufacturer, score, duration) written to SPIFFS for later retrieval.
  Real value for someone who might need to show a record to police or in a
  protective-order filing.

## Phase 3 — High Impact (larger scope, more design/validation risk)

- [ ] **No-reflash configuration.** Allowlist/specialMacs/sensitivity thresholds
  currently require editing `PathShield.ino` and reflashing via Arduino IDE. A
  serial command protocol or temporary WiFi-AP config page would let someone
  configure the device without a toolchain — the single change most likely to
  widen who can actually use this device, and the biggest lift on this list.
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
