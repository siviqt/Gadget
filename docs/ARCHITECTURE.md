# Gadget architecture

Gadget runs on ESP-IDF and FreeRTOS. The launcher installs Gadget on SD and uses its internal AppFS cache to start the core firmware. Gadget owns the application's display and keyboard access and keeps workspace and service state alive across face changes. [SD-card packaging](DEVELOPMENT.md#sd-card-packaging)

`main/` delegates board initialization and the main loop to `gadget_core`. The core presents the startup guide, mounts the SD card through the BSP, starts the module catalog and waits on the runtime queue between updates. The guide discovers its workspace labels from the loaded catalog and can be reopened with `Hyper` + `H`.

## Runtime components

| Component | Responsibility |
|---|---|
| `components/gadget_core` | Board startup, SD package check, event loop and app exit |
| `components/gadget_dice` | Six workspace assignments, focus, rotation state and transition completion |
| `components/gadget_graphics` | Full-screen PAX workspaces, guide, display page flips, cached previews, reel rendering and PPA scaling |
| `components/gadget_input` | BSP event capture, global shortcut matching and active-face event routing |
| `components/gadget_modules` | SD discovery, dependency resolution, BadgeELF loading, focus and worker lifecycle |
| `components/gadget_services` | Shared event queue, RTC time, HTTP(S), focused IMU sampling and exclusive PCM output |
| `components/gadget_config` | Versioned workspace and module settings, plus background SD writes with pending and backup copies |
| `components/gadget_sdk` | Public contracts, capability discovery, lifecycle and compatibility checks |

## Event and rendering flow

The input task maps the Tanmatsu BSP's Super modifier to `Hyper`. A standalone tap starts a two-second window for one `F1`–`F6` selection. Other keypresses cancel the window. The task accepts held chords, validates two distinct `Esc` presses within 850 ms for launcher exit, and routes the physical volume buttons to Radio from every workspace. The main task maps workspace shortcuts to configured reel slots and forwards bare function keys, navigation and text input to the focused module. The guide uses the same workspace key mapping.

The reel moves the visible cards downward when advancing through slots 1–6, wrapping between slots 6 and 1. Gadget chooses the shortest path from the last displayed position and moves down on a tie. It zooms uniformly, keeps card content upright and focuses the final destination at the end of the transition.

The focused face draws into a native panel page when its state changes. Each scanout page keeps its own pending damage so animated regions can reuse static graphics. Gadget caches a half-resolution image when leaving a face. Modules can opt in to a preview render at startup with `startup_preview`. The reel uses bilinear RGB565 sampling. PPA scales and rotates the animation into a scanout page, with a CPU path for panels that cannot use PPA. The graphics manager waits for two refresh edges after each flip before reusing the old page.

Services publish state changes through a bounded queue. Face callbacks receive those events on the runtime task and invalidate their canvas as needed. Workspace updates draw the active face, and the motion sampler runs while a motion-capable face is focused. On exit, Gadget joins the workers, stops services, unloads modules in reverse startup order and flushes pending settings before unmounting the SD card.

Gadget samples heap, CPU and stack headroom on the runtime task about every five seconds. It records reel latency after the destination face has been presented. The System module reads the latest snapshot and the live states of Wi-Fi, PCM and loaded services through the SDK. Serial logging captures measurements while any face is active. [Hardware validation](DEVELOPMENT.md#m4-hardware-validation)

## Module storage and loading

Every feature module ships with a manifest, executable and any assets under `/sd/apps/<slug>/modules/`. The launcher passes the SD app path in the AppFS 2.1.0 boot handoff. Gadget reads the retained path through ESP-IDF and checks the SD folder for the app metadata and core image before scanning for modules. If the card or app directory is unavailable, Gadget reports the problem and offers a return to the launcher. A rejected package produces a serial diagnostic and an unassigned workspace remains available as a placeholder. [AppFS boot argument layout](https://github.com/badgeteam/esp32-component-appfs/blob/684af283b0ce22cef90239c8cfa82be4bbe8fdc6/include/appfs.h)

The package manifest declares the module ID, name, version, supported SDK version, kind, required capabilities, dependencies and settings. Gadget checks compatibility before loading module code and registering callbacks. Initial kinds are Face, Service and Combined. A combined module supplies both a face and a service with independent lifetimes.

- `init` acquires module resources. `shutdown` releases them even after partial startup.
- `activate` and `deactivate` handle focus changes. Workspace state survives a focus change.
- `update` handles scheduled work or events, `render` draws into the assigned surface and `input` handles routed events.
- Service `start`, `stop` and event processing follow service lifetime independently of focus. Stop and drain workers before releasing shared resources.

The BadgeELF loader uses Gadget's own file, memory and cache port. Modules are position-independent ESP32-P4 ELF files, and the entry point returns a versioned Gadget callbacks table. The sample face and service build independently of the core firmware. [Module SDK v1](MODULE_SDK.md)

Workspace overrides reference stable module IDs and take priority over the manifest's default slot. A combined module can run its service even when the layout leaves its face out of the reel. Layout and per-module state are scoped under `/sd/data/<slug>/` so the launcher can replace the install directory independently. [Workspace settings](DEVELOPMENT.md#workspace-settings)

## Radio playback and directory

Radio uses separate workers for stream download, decoding and Radio Browser search. The directory worker discovers mirrors using DNS SRV with fixed fallbacks and reads paged results over HTTPS. It looks ahead through up to three four-entry batches and presents supported direct MP3 and AAC streams before HLS entries in those batches. Cursor history preserves backward navigation through pages of results. The stream worker requests ICY metadata, strips the metadata blocks from the audio, and stores track titles for the decoder to publish as it reaches their stream positions. Download and decoding share a 256 KiB compressed-audio buffer. [Shared service APIs](MODULE_SDK.md#shared-services)

The decoder acquires the shared PCM output and queues signed 16-bit stereo frames in a separate 256 KiB buffer. It estimates the compressed bitrate from one second of decoded audio for streams without a reported rate. Gadget prebuffers audio, drains it to I²S, and manages the headphone or speaker route and volume. Closing buffered output silences and disables I²S DMA. Muting adjusts output gain while download and decoding continue. [Radio controls and validation](DEVELOPMENT.md#internet-radio)
