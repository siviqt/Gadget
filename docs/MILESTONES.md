# Implementation milestones

| Milestone | Depends on | Status |
|---|---|---|
| M0 · Workspace and AppFS starter | Existing ESP-IDF installation | Complete on 2026-11-19. [Validation record](DEVELOPMENT.md#starter-validation) |
| M1 · Runtime and Gadget Dice | M0 | SD deployment, interactive test and transition timing captured. The repository installer test is open. |
| M2 · Module SDK and service manager | M1 | Implemented. SD module load, focus, background service and worker shutdown verified. [Validation record](DEVELOPMENT.md#module-sdk-validation) |
| M3 · Four native reference modules | M2 | Complete on Gadget 0.3.2. Gadget 0.3.3 adds the reel, one-handed switching and workspace guide. [Revision 6 validation](DEVELOPMENT.md#workspace-reel-and-guide-checks) |
| M4 · Configuration and hardware validation | M3 | Implemented in 0.4.0. SD installation and the stable Wi-Fi baseline passed. Handheld recovery, concurrency and power checks are open. [Hardware validation](DEVELOPMENT.md#m4-hardware-validation) |
| M5 · Developer SDK and first release | M4 | Implemented in 0.5.0, SD revision 8. Clean build, package validation and SDK loader checks passed on the host. Tanmatsu release checks are pending. |

## M0 · Workspace and AppFS starter

- Set up the Gadget-owned ESP-IDF project, ESP32-P4 configuration, version pins and AppFS workflow.
- Provide an interactive smoke app for the display and keyboard with a return to the launcher.
- **Acceptance** — Build from defaults, inspect the ESP32-P4 app image and record the result. On a Tanmatsu, install through BadgeLink, see the upright display, confirm keyboard input, exit and relaunch. Follow the [M0 starter smoke test](DEVELOPMENT.md#m0-starter-smoke-test).

## M1 · Runtime and Gadget Dice

- Move board lifecycle, input and graphics ownership into Gadget runtime components. Add six selectable workspaces with basic placeholders in the core.
- Create the SD-only package and its `metadata.json`. Require the SD app directory at startup. [SD-card packaging](DEVELOPMENT.md#sd-card-packaging)
- Render workspace transitions in Gadget and use PPA for scaling and panel rotation where available.
- Bind `Hyper + F1` through `Hyper + F6` after verifying the actual BSP input mapping. Keep a bounded preview cache and return to event-driven PAX drawing after the transition.
- **Acceptance** — Confirm the repository installer requires a mounted SD card and first launch caches the SD-installed core in AppFS. Record the core binary size. Verify that each shortcut reaches its intended face and that rapid transitions settle on the last requested face and route focus correctly. Record transition timing and peak framebuffer memory.

Gadget 0.1.0 passed the interactive check on Tanmatsu with Gadget's original PAX cube renderer. See the [build, SD and device verification record](DEVELOPMENT.md#gadget-dice-validation).

## M2 · Module SDK and service manager

- Select the module loader and executable format. Discover packages under the Gadget SD app directory from versioned manifests, and check capabilities and dependencies before loading. Register lifecycle callbacks, route input and provide render surfaces.
- Add shared event delivery and start/stop handling for services, including cooperative worker shutdown and shared access to time, network and audio.
- Package a Hello Gadget face and a sample service on SD. The service updates while focus is elsewhere.
- **Acceptance** — Verify that adding or removing an SD package changes discovery on the next launch with the same Gadget core image. Reject duplicate IDs, incompatible SDK versions and unavailable required dependencies with useful diagnostics. Verify focus changes, state retention, independent service updates and cleanup after partial startup.

Gadget 0.2.0 runs the Hello Gadget face and Pulse Service from independent SD packages. The [SDK contract](MODULE_SDK.md) covers module builds and shared time, HTTP(S) and PCM operations.

## M3 · Four native reference modules

Gadget 0.3.2 completes M3. The SD package contains the four native modules, their license notices, and the follow-up improvements to settings persistence, Radio, workspace rendering and launcher exit. The user confirmed that the build passed their Tanmatsu checks. [Validation record](DEVELOPMENT.md#native-module-validation)

| Module | Deliverable | Acceptance |
|---|---|---|
| Clock | Time, date and calendar face with RTC and time-zone support | Time and calendar remain correct across a face change and a date boundary. Invalid or unavailable time is shown clearly. |
| Eyes | Interactive face with animated expressions | Motion drives gaze and expressions, which resume after a face switch. Inactive animation avoids continuous rendering. |
| Internet Radio | Player face and independently running audio service | Start playback, visit other faces and return to the same stream and controls. A network drop is surfaced and recovery follows service state. |
| Weather | Current conditions and forecast face with a refresh service | A refresh started from another face updates the cached view. Offline and missing-location states remain usable, and reconnect refreshes stale data. |

Each module ships its executable and assets under `modules/` in the SD app directory. Radio can play during workspace rotations and Weather refresh. The source revisions and licenses are recorded in the [reference inventory](REFERENCE_SOURCES.md).

Gadget 0.3.3, Tanmatsu SD revision 6, gives each face the full panel and replaces the cube with a vertical reel. A standalone `Hyper` tap arms the next function key for two seconds. The startup guide lists loaded faces and global controls, and `Hyper` + `H` reopens it. [Workspace reel and guide checks](DEVELOPMENT.md#workspace-reel-and-guide-checks)

## M4 · Configuration and hardware validation

- Persist face assignments, shortcuts and module settings with Gadget-scoped keys and a versioned schema. Recover to defaults if settings are missing, incompatible or invalid. Keep larger mutable state on SD outside the app directory that the launcher replaces on update.
- Add a system face for resource and service status. Measure internal heap, PSRAM, task stack high-water marks, idle and active CPU usage, transition latency and power draw on the handheld.
- **Acceptance** — Settings survive restart and recover predictably from missing and malformed data. Repeated transitions, concurrent services and network loss meet resource and responsiveness budgets established from device measurements. Document tested board, panel, chip and firmware combinations.

Gadget 0.4.0 adds the workspace layout editor to the guide and ships the System module in the sixth default slot. [Workspace settings](DEVELOPMENT.md#workspace-settings) · [M4 test record](DEVELOPMENT.md#m4-hardware-validation)

## M5 · Developer SDK and first release

- Publish the SDK contract, scaffold, `examples/hello_gadget`, build and contribution instructions, release artifacts and third-party notices. Include the four reference-derived modules as examples.
- **Acceptance** — Third-party face and service packages build against the documented SDK and load from SD. A clean checkout produces an SD-only package with the AppFS core image, metadata and modules, and passes the device release checks. Document launch, update and return-to-launcher behavior.

[SDK quick start](MODULE_SDK.md#build-and-install) · [Release checks](DEVELOPMENT.md#m5-release-validation)
