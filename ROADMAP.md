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

## Phase 4 — On-hardware UX corrections (from field testing)

Everything above was designed before the S3 port was stable enough to live with.
Once it was, three things turned out to be wrong in practice rather than on paper.

- [x] **Rebuilt the control scheme around tap vs hold on one button.** The
  settings menu was previously behind an A+B chord, which was effectively
  unreachable: whichever button went down first had already fired its own
  action (pause, or a filter cycle), so the chord almost never registered as
  a chord. The chord is gone. Every gesture is now a tap or a ~1s hold on a
  single button — tap A scrolls, tap B filters, **hold A stops and starts
  scanning** (the same button gates both directions, instead of stop-on-A /
  resume-on-B), **hold B opens the settings menu**. Menu nav on A stays
  hold-free so repeated presses stay instant. Hold detection returns the
  moment the button comes up, so taps have no added latency — only an actual
  hold costs the wait.
- [x] **Made the control scheme discoverable.** A full cheat sheet is drawn on
  the device: shown for a few seconds at boot (dismissable), available any
  time from a new **Show Controls** menu row, and printed over serial at boot
  and via a `controls` command. The findings-screen footer now permanently
  carries the two gestures nobody can guess at (`HOLD A:Stop`, `HOLD B:Menu`)
  alongside the active filter. Nothing about the button scheme should require
  reading the README.
- [x] **Alert style is now three explicit options, defaulting to the calm one.**
  The Phase 2 alert work made "loud" mean a 5x full-screen red/blue strobe and
  offered only a near-invisible quiet mode as the alternative — nothing in
  between, and the strobe was the default. Alert Style now cycles **Simple**
  (one solid red screen, no animation — the new default) → **Quiet**
  (border only, unchanged) → **Loud** (the strobe, now opt-in). Sound split
  back out into its own menu row: folding it into the same cycle made a
  4-state toggle whose next state nobody could predict.
- [x] **Filters pin the view instead of fighting the band switch.** A filter
  only ever applied to the BLE list, but the screen kept alternating to the
  unfiltered WiFi list every 3 seconds — which read as the filter switching
  itself off and back on every few seconds. With a filter active the display
  now stays on the BLE list; scanning still alternates bands underneath, only
  the view is pinned. A filter matching nothing now says so instead of
  rendering a blank screen.
- [x] **Allowlisting moved from a hidden gesture to a menu row.** Hold-A on the
  paused list (Phase 2) collided with hold-A's new stop/start job. It's now an
  **Allowlist Top Device** menu action, which is also strictly more
  discoverable than an undocumented long-press — and the row displays the tail
  of the MAC it will act on, so it's a confirmable choice rather than a blind
  one. Behaviour is otherwise identical (exact-MAC, BLE-only, persisted to
  `/allowlist.txt`).

## Phase 5 — Render quality, alert availability, and the WiFi detection gap

A code review of the whole firmware turned up three problems that are less about
missing features than about the device not doing what it already claims to do.
Phase 4 fixed what field testing exposed; these are what reading the code exposed.

- [ ] **Flicker-free rendering via a PSRAM sprite.** Every render did
  `M5.Display.fillScreen(BLACK)` and then repainted the whole screen field by
  field, once a second — a visible black flash on every update, on the findings
  list, the top bar and the settings menu alike. The repainting screens now draw
  into a 240x135x16bpp `M5Canvas` held in PSRAM (~65KB, which the S3 has in
  abundance and which doesn't touch the internal heap the MEM bar tracks) and
  reach the panel as a single `pushSprite()`. Redraws also get *faster*: one bulk
  SPI transfer instead of a few hundred small ones. Scoped to the screens that
  repaint on a timer — the boot splash, controls sheet, feedback toasts and alert
  screen are drawn once and left up, so they never flickered and stay on direct
  rendering rather than being churned for consistency's sake.
- [ ] **An alert no longer blinds the device until someone presses a button.**
  `alertUser()` blocked on a button press with no timeout while `scanTask` spun on
  `showingAlert`, so the moment PathShield detected something was the moment it
  stopped looking — indefinitely, if it was in a pocket. For an anti-stalking tool
  that is exactly backwards. The alert now auto-dismisses (the `ALERT_DURATION`
  constant had been declared and left unused since the stability pass) and scanning
  resumes. Because a dismissed alert shouldn't vanish without a trace, the findings
  screen draws its border in red with an `!N` count while any alert is
  unacknowledged, clearing once the user looks at the ALERTS filter. The border was
  chosen over a banner row deliberately: at 240x135 the footer is already carrying
  the filter tag, both hold hints and the page counter, and the screen edge was
  otherwise decorative.
- [ ] **WiFi BSSIDs are matched against the special-MAC list.** `isSpecialMac()`
  was only ever called from `trackDevice()` — the BLE path. The Axon and Flock
  OUIs that ship in `defaultSpecialMacs[]` therefore never alerted over WiFi,
  despite the README billing them as "Privacy Invader Defaults" and the web
  flasher's threat matrix listing `Axon TASER | WiFi/BLE | DETECTED`. Flock
  cameras beacon over WiFi. This closes the largest gap between what the project
  documents and what it does. Carries two necessary companions: the allowlist
  (both compiled-in prefixes and the runtime list) now applies to WiFi BSSIDs
  too, since otherwise a WiFi false positive would have no off switch; and
  alerting WiFi entries survive `removeOldWiFiEntries()`, matching how the BLE
  list already preserves `alertTriggered` devices.
- [ ] **The ALERTS filter covers both bands.** Follows directly from the item
  above: the filter pinned the view to BLE, so a WiFi hit would have been
  invisible under the filter named "Alerts". It now renders BLE and WiFi
  alerting devices as one combined, band-tagged list.

## Phase 6 — Second review pass

Smaller corrections from the same review, plus the two documentation gaps it
exposed.

- [x] **Stopping merges the two lists.** `isWifiView()` keyed off `scanningWiFi`,
  which is frozen while paused — so pausing during a BLE window pinned you to
  BLE with the WiFi list unreachable until you resumed. While stopped there is
  no band alternation to follow, so the view now shows one combined list, each
  row tagged `[BLE]`/`[WiFi]`, reusing the merged renderer built for the ALERTS
  filter. Fixes the band problem without inventing a gesture — every button is
  already assigned in both scanning and paused modes.
- [x] **Scrolling advances a page, not a row.** At three rows per screen,
  stepping by one meant 67 taps to walk a 70-device list, re-rendering two rows
  the reader had already read each time.
- [x] **Battery percentage via `M5.Power.getBatteryLevel()`, smoothed.** The old
  `(V - 3.0) / 1.2` lerp treats a lithium discharge curve as a straight line, so
  it read high for most of a session and then fell off a cliff; unfiltered
  samples redrawn every second also visibly jittered. Now uses M5Unified's
  curve with an exponential moving average, in one helper shared by the top bar
  and the settings screen instead of duplicated between them. The
  critical-battery shutdown moved out of `drawTopBar()` into `loop()` — a render
  function had no business powering the device off, and it did so while holding
  `deviceMutex` across a 3-second delay.
- [x] **Signal strength as bars or dBm, switchable.** A new **Signal Display**
  menu row cycles BARS/dBm. Bars answer "is it getting closer?" at a glance;
  dBm is what you want comparing two devices or writing a finding down — so
  it's a preference, not a replacement. Persisted to `/prefs.txt`.
- [x] **Inverse-video menu selection.** A single `>` in the margin is easy to
  lose at this size; the highlighted row now reads from across a room. Row
  height dropped to 9px to fit the tenth option in 135px.
- [x] **"You got an alert — now what?" in the README.** For an anti-stalking
  tool the response guidance matters as much as the detection, and there was
  none: no way to tell a commuter from a follower, no capture-then-report
  sequence, and nothing saying plainly that a quiet screen isn't proof of
  safety. Leads with the boring explanations, because most alerts are the
  user's own earbuds.
- [x] **Web flasher accessibility and metadata.** The body-wide opacity flicker
  and sweeping scanline now stop under `prefers-reduced-motion` — persistent
  motion of exactly the kind that triggers migraine and vestibular symptoms, on
  a page whose only job is one button. Added a description, favicon and OG tags
  so shared links preview as something; replaced the stale hardcoded "2024".

### Still open

- [ ] **Per-device detail view.** Wanted, but every gesture is assigned in both
  scanning and paused modes, so it needs either a repurposed button, a menu row
  (the precedent Phase 4 set when it moved allowlisting off a hidden gesture),
  or a change to what "paused" means. Under discussion.
- [ ] **Move rendering out from under `deviceMutex`.** `displayTrackedDevices()`
  holds the mutex across every SPI write. `scanTask` needs the same mutex to
  record results and gives up after 2000ms, so a slow frame can make an entire
  scan batch get dropped. Fix is to copy the ≤3 visible rows under the lock,
  release, then draw. The O(n²) per-frame bubble sort (~2,400 comparisons at 70
  devices, re-run every frame inside that same lock) is the same problem and
  should be made incremental at the same time — it matters mainly because of
  how long it holds the lock.
- [ ] **IMU-based motion correlation.** Carried over from Phase 3.

## Already completed (context, not part of this roadmap's phases)

The stability pass that preceded this roadmap: fixed the Core0/Core1 display-and-button
race condition causing freezes and unresponsive buttons, re-armed the task watchdog,
fixed a bug where simultaneous new-tracker detections in one scan batch could silently
drop all but the last alert, dropped M5StickC Plus 1.1/Plus 2 support to focus this fork
on the S3 exclusively, and removed the dead `/devices.txt` write-only persistence. See
PR history for detail.
