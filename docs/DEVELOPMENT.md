# Development and SD-card installation

## Build environment

Run commands from the Gadget source checkout. The wrapper activates the local ESP-IDF 6.0.2 installation and uses the matching RISC-V toolchain and Python environment.

```bash
./tools/idf.sh --version
./tools/idf.sh build
./tools/idf.sh size
```

For a different installed toolchain location, set `IDF_PATH` to the ESP-IDF checkout and `IDF_TOOLS_PATH` to its tool directory. ESP-IDF selects the Python environment from the tool directory by default. For a separately located environment, pass `IDF_PYTHON_ENV_PATH` as well. The build host needs Bash, Git, CMake 3.22 or newer, Ninja and a supported Python version. Install any new toolchain files under Gadget's `.cache/` to keep setup within the project. [ESP-IDF setup for ESP32-P4](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32p4/get-started/)

`sdkconfig.defaults` provides the board and runtime configuration. `main/idf_component.yml` pins the direct dependencies, and `dependencies.lock` records the resolved versions and content hashes for ESP32-P4. Keep all three files with the source. A build from the source-only tree may need access to the Espressif Component Registry to restore the pinned components.

ESP-IDF generates `build/`, `sdkconfig` and `managed_components/`. The wrapper keeps its temporary files and component cache under `.cache/`. The packager writes the SD package under the selected build directory. To verify a fresh configuration and stage its package, use a new build directory.

```bash
./tools/idf.sh -B build/verify -DSDKCONFIG="$PWD/build/verify/sdkconfig" build
python3 -B tools/package_sd.py --build-dir build/verify
```

| Build component | Gadget 0.5.0 |
|---|---|
| ESP-IDF and FreeRTOS | ESP-IDF 6.0.2 and its bundled FreeRTOS |
| RISC-V toolchain | Espressif GCC 15.2.0, `esp-15.2.0_20251204` |
| Board support | Badge BSP 2.0.0, MIPI DSI Abstraction 2.0.0 |
| 2D graphics | PAX Graphics 2.2.2 |
| Workspace reel | Gadget's RGB565 renderer with ESP-IDF 6.0.2 PPA scaling and rotation |
| Module loading | BadgeELF 0.8.0 with Gadget's platform port, and cJSON 1.7.19~2 |
| Shared network services | Tanmatsu Wi-Fi 1.3.1 and Wi-Fi Manager 0.3.0 |

The supplied Tanmatsu template pins Badge BSP 1.6.4. Gadget uses 2.0.0 to align the build with the proposed 2.0.0 compatibility baseline. Gadget takes its initial ESP32-P4 silicon configuration from the template and its partition layout from the supplied launcher v0.3.1. [Tanmatsu application template](https://github.com/Nicolai-Electronics/tanmatsu-template) · [Consolidated license](../LICENSE)

## Device compatibility to verify

| Area | Target and current coverage |
|---|---|
| Tanmatsu board | All board revisions are the project target. Test each available board and LCD variant and record the board, panel and chip revisions. |
| ESP32-P4 silicon | The default profile produces an image for v0.1–v1.99. M4 was measured on v1.3. ESP-IDF uses a separate profile for v3.x. Use the installed device's chip revision when selecting a profile. |
| Launcher | v0.3.0 and v0.3.1 both use the AppFS 2.1.0 handoff and pass the SD app path. Verify SD-only installation and caching on each launcher version tested. |
| Radio firmware | v3.4.0 or newer is the network service target. |
| Management coprocessor | r9 or newer is the input and board-management target. |

The operating system on the build host supplies developer tools. Gadget runs on ESP-IDF and FreeRTOS on the handheld.

## SD-card packaging

The Tanmatsu entry in `metadata.json` sets `external_only` to require a mounted SD card in the repository installer. The install uses the app repository's folder name as the slug. [Tanmatsu app repository metadata](https://github.com/Nicolai-Electronics/app-repository#application-repository)

```bash
./tools/idf.sh build
python3 -B tools/package_sd.py
```

The packager stages `metadata.json`, `gadget.bin`, three icons, one consolidated `LICENSE` and seven loadable modules under `build/sd/apps/local.gadget/`. Copy the staged app folder into `apps/` on the Tanmatsu SD card. BadgeLink exposes the folder as `/sd/apps/<slug>/`. Open **Gadget** with the cyan cube icon to start it through the launcher.

For a repository package, pass the intended repository folder to `--slug`. Put the approved public author and source link in a JSON file under the ignored `.cache/` directory with the keys `author` and `repository_url`, then pass that file to `--release-metadata`. The packager writes those fields to the packaged `metadata.json`. The release build supplies the ESP32-P4 core and the modules, and its `LICENSE` includes the bundled firmware dependency notices. [Consolidated source license](../LICENSE)

List `LICENSE` and each packaged module manifest, executable and asset in the Tanmatsu entry's `assets` array. Use `source_file` for the path within the package and `target_file` for the path relative to the installed app directory. Module files install under `modules/<folder>/`. The module's ID is set by `module.json`. Increment the target's `revision` when publishing a new firmware image so the launcher can refresh its cached copy. [Repository package fields](https://github.com/Nicolai-Electronics/app-repository#application-repository)

The launcher copies `gadget.bin` from SD into its internal AppFS execution cache on first launch. It passes the installed app path to Gadget. Gadget uses the same slug for `/sd/data/<slug>/`, so updating an existing app folder preserves its layout and module state. For a manual update, replace the app folder using the same slug and use **F4 Uncache** if the launcher offers it before starting the app again. [Workspace settings](#workspace-settings) · [Launcher app management](https://github.com/Nicolai-Electronics/tanmatsu-launcher/blob/main/main/app_management.c)

## Gadget Dice validation

On the device, visit all six faces with `Hyper` + `F1` through `F6`. Request several faces in quick succession and check that the last face stays in focus. Press `Enter` on an unassigned face, switch away, and return to verify that the input confirmation was retained. Press `Esc` twice to return to the launcher.

Both default and fresh ESP32-P4 builds produced a 552,512-byte Gadget 0.1.0 image. Host checks exercised all 36 source and destination face pairs and 200 interrupted retargets. The package matched the repository schema and all six files matched the local artifacts when read back from SD over BadgeLink. The release slug was absent from AppFS at the end of that transfer.

On 2026-11-19, the connected Tanmatsu booted Gadget from the SD installation and logged focus on `F1 Clock`. The user confirmed the on-device check passed for the six shortcuts, rapid switching, `Enter` focus and `Esc` back to the launcher.

The device logged 1,536,000 bytes for two 480 × 800 RGB565 scanout pages, 155,520 bytes for the six previews and 192,000 bytes for the half-resolution animation surface. In the 0.2.0 on-device check, 14 transitions reached focus in 421–536 ms after the last selection. All 44 captured animation frames used PPA scaling, with peak scale times of 5.98–6.16 ms per transition. Testing through the repository installer is pending.

## Module SDK validation

Default and fresh ESP32-P4 builds produced a 1,291,856-byte Gadget 0.2.0 core. Both sample modules built as separate RV32 ELF files. All ten package files matched the copies read back from SD over BadgeLink. The previous SD package was saved at `/sd/gadget-0.1.0-backup`.

Host checks exercised the production manifest and catalog logic with loader and RTOS stubs. The same test binary discovered, hid and rediscovered a face package on separate catalog startups. The checks also covered dependency ordering, duplicate IDs, incompatible SDK versions, unknown capabilities, absent and cyclic dependencies, and cleanup after failed initialization or service startup.

On Tanmatsu, the launcher started Gadget 0.2.0 from the SD installation. The serial trace recorded two SD modules loaded, RTC time available to Pulse Service and a background tick while Clock was focused. The user confirmed that Hello Gadget retained the Enter count through a focus change while the tick count advanced. `Esc` joined the worker, unloaded both ELFs and returned to the launcher. The native-module validation below covers the later Wi-Fi and PCM integration. [Module SDK v1](MODULE_SDK.md)

## Native module validation

The user confirmed that Gadget 0.3.2, Tanmatsu SD revision 5, passed their device checks. This completes M3. [Milestone tracker](MILESTONES.md)

The default and fresh ESP32-P4 builds produced a 1,294,160-byte Gadget 0.3.0 image. The image checksum and validation hash passed. All six module ELFs passed the standalone RV32 format checks. All 23 package files matched the copies read back from SD over BadgeLink on 2026-11-20. The previous SD installation is at `/sd/gadget-0.2.0-m3-backup`.

Host checks exercised Clock across a date boundary, Eyes Mood across focus changes, Radio through MP3 decoding and simulated network loss, and Weather through a background refresh and offline recovery. The Weather checks used responses from Open-Meteo. On 2026-11-20, the user confirmed that all four workspaces worked on Tanmatsu and reported choppy, faint audio and slow animation.

### Workspace controls and regression checks

Radio and Weather use saved launcher Wi-Fi profiles. Clock uses the launcher RTC and local time zone. Weather starts with an empty location and saves the city after entry. It defaults to a 15-minute refresh interval and retries offline requests after 10 seconds.

| Default workspace shortcut | Controls and device check |
|---|---|
| `Hyper` + `F1` · Clock | Confirm the RTC time and today's highlight. Check that the box is centered over the date and fits within its row. Use Left/Right to browse months, Enter to return to today and U to toggle local time and UTC. |
| `Hyper` + `F2` · Radio | Use Left/Right to select a station and Enter to play or stop. The physical volume buttons work from all workspaces. To search during playback, press S, type a name such as Jazz and press Enter. Use Left/Right for pages and Up/Down to select a result. Press A to save the highlighted or tuned station, Enter to tune a result, and Backspace to return to the player. |
| `Hyper` + `F3` · Eyes | Hold Tanmatsu still briefly, then tilt or turn it to check gaze, expressions and blinks. Check that the eyes fit the full face and resume animating after switching away and back. |
| `Hyper` + `F4` · Weather | Confirm the missing-location screen. Press L, type a city such as Sofia and press Enter. Check current conditions and three forecast days. While Radio is playing and another workspace is focused, press `Hyper` + `R`. Return to Weather and confirm the update time advanced and Radio continued playing. Drop Wi-Fi, request a refresh and confirm that Weather shows the previous forecast while offline and refreshes on reconnect. |
| `Hyper` + `F5` · Hello Gadget | Press Enter to increment the local count. Switch away and return. The Enter count is retained across focus changes, and the Pulse Service count advances in the background. |
| `Hyper` + `F6` · System | Wait for the CPU sample and check heap, stack, reel timing, Wi-Fi, PCM audio and service status. Use Left/Right to page through the service list when it exceeds four entries. |

While both services are active, press `Esc` once and check that Gadget stays open. Hold the key and check again, then release it and press it twice within 850 ms to exit to the launcher. Reopen Gadget and check the Weather city, Clock time preference, saved stations, last station selection and both speaker and headphone levels. Radio starts playback when you press Enter. With serial logs, look for `Playing live`, `Forecast updated` and the worker unload messages.

Gadget saves each module's state under `/sd/data/<slug>/<module-id>/`. The `internet_radio/stations.txt` catalog uses TanRadio's text format. Gadget plays direct MP3 URLs. You can also specify a stream with `settings.station_url` and `settings.station_name` in Radio's installed SD manifest. On first launch, Weather can seed its saved city from `settings.location` in its installed SD manifest. The release starts with an empty Weather location and public Radio presets.

### Tuning validation

The default and fresh builds produced a 1,393,440-byte Gadget 0.3.1 image. The image checksum and validation hash passed. All 23 files matched SD readback over BadgeLink on 2026-11-20. The previous installation is at `/sd/gadget-0.3.0-tuning-backup`.

Host checks exercised MP3 decoding, PCM backpressure, directory paging during playback, station selection and reconnect. The search check used two live Radio Browser response pages. Simulated IMU samples drove the Eyes pose, and clipped redraws matched full renders across all four display orientations. Cube checks covered all 36 workspace pairs and 200 rapid retargets.

The user confirmed smooth Eyes and cube motion on 0.3.1 and reported soft cube textures and underruns on some stations. In 0.3.2, cube transitions use 400×240 previews on the 800×480 display and interpolate texture samples. Radio uses 256 KiB each for compressed MP3 and PCM. After synchronization it decodes complete frames as they arrive. The serial summaries report the cube frame count, p95 presentation interval, peak draw and scale times, and scanout wait. Radio logs compressed and PCM buffer levels, PCM underruns, and output errors every five seconds.

On 2026-11-20, the default and fresh ESP32-P4 builds passed for 0.3.2. BadgeLink verified all 23 files on SD and the previous installation was saved to `/sd/gadget-0.3.1-polish-backup`. Host checks covered 32 kbps MP3 in small increments, station catalog recovery, Weather reload, clean-exit settings flush, simulated double Esc presses, calendar alignment, and cube and Eyes rendering.

#### Playback and rendering regression checks

1. Play a preset and a station that previously stuttered for ten minutes each on stable Wi-Fi. Switch workspaces, refresh Weather with `Hyper` + `R`, and page through Radio search results during the run. Check for zero PCM underruns and audible gaps.
2. Compare speaker and headphone output at the same station and similar volume with TanRadio. Drop and restore Wi-Fi once, then confirm playback recovers and the selected station and volume are retained.
3. Tilt the device repeatedly on Eyes, then hold it still. Target 30 visible frames per second during motion and check for jitter at rest. Check that rapid workspace shortcuts land on the last requested face.
4. Check typography on the clock, Weather and Radio results. Include accented names and long station labels. Record the panel and board revision alongside the serial results.

### Workspace reel and guide checks

On 2026-11-20, the default and fresh ESP32-P4 builds produced a 1,478,080-byte Gadget 0.3.3 image. The image checksum and validation hash passed. Host checks covered all 36 face pairs, 240 reel retargets, the two-second shortcut boundary, guide behavior with vacant workspaces and full-screen rendering across four panel orientations. BadgeLink verified all 23 files of SD revision 6. The previous installation was saved at `/sd/gadget-0.3.2-reel-backup`.

With passive serial monitoring, Gadget switched through all six faces during 56 seconds of Radio playback with zero reported PCM underruns or output errors. Uninterrupted reel transitions used PPA and logged p95 presentation intervals of 68–84 ms while Radio played. The same session captured clean unloading of all six modules, the return to launcher v0.3.1 and a relaunch to Clock.

1. Launch Gadget with the full SD package, then with one optional face module removed. Confirm that the guide displays each loaded name, marks the vacant slot Empty, and opens F1 after about three seconds. Press `Enter` on another launch to continue as soon as the catalog is ready.
2. Hold `Hyper` + `F2` from F1 and check that F2 enters from the top. Try F1 to F6 for an upward wrap, then F1 to F4 and back for downward ties. The faces stay upright and retain their proportions. Retarget during a spin and check that the last selection takes focus.
3. Tap and release `Hyper` and press a function key within two seconds. Try a second function key, an intervening key, and a function key after expiry. Open the guide with `Hyper` + `H`. Press `Enter` to return to the current face, then reopen the guide and press `Esc` twice within 850 ms to return to the launcher.
4. Play Radio while switching faces and while the guide is open. Check for audible gaps and PCM underruns. The display log reports reel frame count, p95 presentation interval, peak drawing and scaling times, and scanout wait.

#### Open stability findings

- One launch rebooted after the ESP-Hosted assertion `assert failed: sdio_rx_get_buffer sdio_drv.c:896 (*buf)`.
- On a separate early exit, the Weather worker missed its 3.5-second shutdown deadline. Gadget logged `ESP_ERR_TIMEOUT` and returned to the launcher.

These findings were recorded on SD revision 6. The proposed changes to start module workers after the startup guide and reset the radio transport on first network use were deferred. The later revision 7 run established a performance baseline for normal operation. [M4 hardware validation](#m4-hardware-validation)

## Workspace settings

Open the guide with `Hyper` + `H` and press `C` to edit the layout. Use Up/Down to select a slot and Left/Right to swap its face with the adjacent slot. Press `S` to edit shortcuts using the same controls, or `D` to restore the manifest defaults and the original key order. `Enter` queues the settings for an SD write. Use `Backspace` to return to the guide and double `Esc` to exit cleanly. The next launch reads the new layout. The guide always shows the active key bindings.

Gadget's layout file is `/sd/data/<slug>/_runtime/settings.json`. To edit it on a host, close Gadget first. The example places Radio in slot 1, Clock in slot 2, and maps `Hyper` + `F3` to slot 6.

```json
{
  "version": 1,
  "settings": {
    "faces": ["internet_radio", "clock", null, null, null, null],
    "shortcuts": [1, 2, 6, 4, 5, 3]
  }
}
```

`faces` lists six reel slots in order. A `null` entry follows the module manifests, an empty string leaves a slot vacant, and a module ID takes priority over the manifest. If a specified module is missing, rejected, or fails to initialize, Gadget uses the manifest default for that slot when available. `shortcuts` lists the six destination slots for `F1` through `F6` and uses every slot number exactly once. Held Hyper and the two-second Hyper tap both use this mapping. Continuing from the startup guide opens reel slot 1.

Gadget reads a valid `settings.json` first, then `settings.json.tmp`, then `settings.json.bak`. With no compatible copy, the layout follows the manifests and `F1`–`F6` select slots in order. The bundled layout is Clock, Radio, Eyes, Weather, Hello Gadget and System. Module settings retain their version 1 JSON files under the module ID. Modules supply their defaults for missing or invalid preferences. Radio's station catalog and other larger module state live in the same SD data tree across app updates.

## M4 hardware validation

Gadget 0.4.0 is Tanmatsu SD revision 7. The default build and a fresh ESP32-P4 configuration produced a 1,487,424-byte core. The ESP32-P4 image checksum and validation hash passed. The verification build after cleanup of the unused sources restored the pinned components from the local registry cache and staged all 25 package files, including seven ESP32-P4 module ELFs.

Host checks exercised the production SD settings writer, clean-exit flush, loading 0.3.x module preferences, unavailable face fallback, malformed and unsupported settings, and recovery from pending and backup files. The runtime check saved a layout through the editor and verified key dispatch and rapid retargeting after restart. The guide, layout and System face passed rendering checks across four scanout orientations and a portrait layout. Radio and Weather recovered from simulated network loss.

### Tanmatsu baseline

On 2026-11-20, BadgeLink read back all 25 staged files byte for byte. The Tanmatsu launcher cached the core from SD on first launch, and Gadget loaded all seven modules. The board ran an ESP32-P4 revision 1.3 with two cores and a 480×800 ST7701 panel scanning RGB565. The startup trace recorded a successful Weather refresh. The measured `gadget.bin` had SHA-256 `d53232375d5c126f49f4c0930424a4bcd92753d12763cee5ee8051b48fe281c1`. The current source includes the layout save message position fix from the subsequent host render check.

The display allocated 1,536,000 B for two scanout pages, 1,152,000 B for the six previews and 192,000 B for the transition surface.

| Measurement | Observed | Initial regression budget for this test device |
|---|---|---|
| Clock idle CPU | 4% on core 0 and 1% on core 1 over 50 seconds after the initial Weather refresh | At most 10% per core after startup traffic settles |
| CPU during Radio playback and workspace changes | 10–49% on core 0 and 2–7% on core 1 across five-second samples | At most 65% per core |
| Internal heap | 283,067 B allocator low-water total and 184,320 B smallest sampled largest block | At least 224 KiB low-water total and 160 KiB largest block |
| PSRAM | 29,873,452 B allocator low-water total | At least 27 MiB |
| Task stack headroom | The lowest sampled Gadget task watermark was 2,288 B on the keyboard task | At least 1 KiB per measured task |
| Reel | 18 settled transitions through presentation took 367–613 ms and reached all six slots. Per-transition p95 presentation intervals were 68–86 ms. | At most 700 ms to settle and 110 ms p95 between frames under the same workload |
| Audio | 57 five-second PCM reports spanning 280 seconds with zero underruns or output errors during playback and switching | Zero underruns and output errors on stable Wi-Fi |

The baseline covers this board and panel combination on stable Wi-Fi. Handheld layout persistence and malformed-data recovery, concurrent refresh during Radio playback, Wi-Fi loss and recovery, and power draw remain open. Record the PCB revision and launcher, radio and management coprocessor firmware versions alongside those runs.

### Measurement procedure

Record the Tanmatsu board revision, LCD model, ESP32-P4 revision, SD revision, and launcher, ESP32-C6 radio and management coprocessor firmware for each run. The launcher's **Device information** screen shows the board and firmware revisions. Gadget logs the chip revision and native panel dimensions at startup.

1. Start with an unconfigured layout. Open System with `Hyper` + `F6` and confirm that internal heap, PSRAM, CPU and service states populate. Save a face swap and a function-key swap, exit, then relaunch. Check that the guide, held chords, Hyper tap and destination focus agree. Restore defaults in the editor. Confirm existing Clock, Radio and Weather preferences also survive relaunch.
2. With Gadget closed, save a copy of `_runtime/` and test fresh launches with the layout absent, invalid JSON, an unsupported version, a duplicate face ID and a duplicate shortcut destination. Verify fallback to a valid `.tmp` or `.bak` if present, and to defaults when all copies are absent or invalid. Restore the saved directory after the check.
3. Capture the USB serial `gadget_metrics` lines for 60 seconds on an idle face. Repeat while Radio plays and Weather refreshes from another face. Switch through all six faces repeatedly, including rapid retargets, and check that focus settles on the final selection. Record the low-water heap values, largest remaining blocks, task stack high-water marks, per-core CPU and selection-to-presentation latency. Compare both workload windows and the tails of the transition distribution. Collect PCM underrun counts alongside the resource readings.
4. Interrupt and restore Wi-Fi while both network services are active. Check that System reflects the network change, Weather continues to show its last forecast, Radio recovers after reconnect and double `Esc` joins the workers and returns to the launcher.
5. Measure input power with an inline USB power meter at a fixed backlight setting and a stable battery charging state. Record voltage, current and power over 60-second idle and concurrent-service windows, plus peak power during repeated transitions. Record the brightness, charge state, audio output and Wi-Fi conditions with the readings.

CPU busy percentages use per-core FreeRTOS idle-counter differences over the sampling interval. The stack log reports the minimum unused bytes since each Gadget task started. The heap low-water values are the sums of the allocator's per-region minima. Reel timing starts when the runtime accepts the last selection and ends when the destination page has been presented. The System face shows the latest sample, and serial sampling continues when another workspace is focused.

## M5 release validation

Gadget 0.5.0 uses Tanmatsu SD revision 8. Use a fresh source copy to build the ESP32-P4 image and generate the package. Check the image integrity and each module's RV32 ELF format. Verify the metadata against the app repository schema and compare its executable, icons and assets to the packaged files. Check the source and package contents, including firmware strings, for private data before copying them to either release checkout.

The source-only build produced a 1,487,536-byte core with a valid ESP32-P4 checksum and validation hash. Both the development and repository packages passed the official metadata validator. Host checks covered the retained launcher path, and the production loader opened seven bundled modules and two independently scaffolded modules under RISC-V emulation. The external face rendered and the external service published an event and stopped on request. The Tanmatsu checks below are pending.

### Tanmatsu release checks

1. Open the repository entry with the SD card absent and check that installation requires the card. Mount SD and install. Verify the app folder, three icons, consolidated license and seven module packages on the card.
2. Launch Gadget. Check that the launcher caches and starts the ESP32-P4 core, the startup guide lists all six faces, and Hello Gadget's Enter count and Pulse Service ticks advance. Use the [SDK quick start](MODULE_SDK.md#build-and-install) to install a separately built face and service. Relaunch and verify the face renders and the service appears in System.
3. Save a layout change, update a previous installation using the existing slug, and reopen Gadget. Verify the launcher uses the new revision, the saved settings remain available, and Radio and Weather still use their saved settings. Try a launch with the SD app folder unavailable and check the on-screen recovery prompt.
4. Press `Esc` twice within 850 ms and confirm the launcher returns after Gadget stops its workers and flushes settings. Start Gadget again and verify the saved layout.

## M0 starter smoke test

The Gadget 0.0.1 smoke test used BadgeLink to list AppFS entries, upload the starter image directly and launch it. The test checked the upright display, the **Keyboard input received** response to an arrow key, and return to the launcher with `Esc`. Current Gadget installations follow the SD package workflow. [Starter validation](#starter-validation) · [SD-card packaging](#sd-card-packaging)

## Starter validation

On 2026-11-19 the default build and a fresh ESP32-P4 configuration both produced a 430,544-byte Gadget 0.0.1 image. `esptool image-info` verified the target, image checksum and validation hash. The partition layout matched the supplied v0.3.1 launcher.

BadgeLink installed the image as `gadget` (AppFS version `0`). The installed entry appeared in the AppFS listing, and its readback matched `build/gadget.bin` byte for byte.

On 2026-11-19, the user confirmed that Gadget 0.0.1 passed the on-device smoke test on their Tanmatsu.
