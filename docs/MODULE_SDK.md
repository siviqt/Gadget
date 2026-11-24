# Gadget Module SDK v1

Build each face, service or combined module as an ESP32-P4 position-independent ELF. Gadget loads it from `/sd/apps/<slug>/modules/<folder>/` at startup. Each folder contains `module.json`, its executable and any package assets. The folder name and the manifest's module ID are independent.

## Package manifest

| Field | Contract |
|---|---|
| `format` | Integer `1` for the package manifest format |
| `id`, `name`, `version` | Unique lowercase ID, display name and module version. IDs may contain lowercase letters, digits, `.`, `-` and `_`, start with a letter, and fit in 31 bytes. Names fit in 39 UTF-8 bytes and versions in 23 bytes. |
| `sdk` | Integer `1` for the Gadget module ABI |
| `kind` | `face`, `service` or `combined` |
| `face` | Default reel slot `1`–`6` for face and combined modules. The workspace layout can override it by module ID. |
| `startup_preview` | Optional boolean for face and combined modules. Set to `true` when `render` can draw once on a panel-sized canvas after initialization, before the first `activate` or `update`. Defaults to `false`, which uses a labeled preview until Gadget captures a displayed image of the face. |
| `executable` | Filename within the module folder, typically `module.elf` |
| `capabilities` | Array of required capabilities selected from `time`, `network`, `audio`, `storage` and, for face modules, `motion` |
| `dependencies` | Array of required module IDs. Gadget starts prerequisites first. |
| `update_ms` | Optional face update interval in milliseconds, from 10 to 60000 |
| `worker_stack` | Optional service worker stack size from 8192 to 32768 bytes, in multiples of 16. Defaults to 8192. |
| `settings` | Optional module-owned JSON object with integer and string settings. |

The startup catalog holds up to 16 valid manifests and eight dependencies per manifest. Gadget rejects duplicate IDs, incompatible SDK versions, unavailable capabilities and unresolved dependencies before calling the module. A module that fails startup is cleaned up, and independent modules can continue. See the [face manifest](../examples/hello_gadget/module.json) and [service manifest](../examples/pulse_service/module.json).

## Build and install

Use [gadget_module.h](../components/gadget_sdk/include/gadget_module.h) as the contract. `gadget_module_entry()` returns a static `gadget_module_t` table with `abi = GADGET_MODULE_ABI` and `size = sizeof(gadget_module_t)`. The runtime passes the module a context with its host callbacks, SD package path and module-owned state pointer.

With `riscv32-esp-elf-gcc` from ESP-IDF 6.0.2 on `PATH`, create and build a face and a service independently of Gadget firmware. Pass `--cc` to `build_module.py` to select a compiler by path.

```bash
python3 -B tools/new_module.py face example.face build/example_face --name "Example Face" --slot 5
python3 -B tools/build_module.py -o build/example_face/module.elf build/example_face/module.c
python3 -B tools/new_module.py service example.service build/example_service --name "Example Service"
python3 -B tools/build_module.py -o build/example_service/module.elf build/example_service/module.c
```

Each folder now contains `module.json`, `module.c` and `module.elf`. Copy the manifest and ELF, along with any assets the module reads, into its folder under `/sd/apps/<slug>/modules/`. With a full default layout, assign `example.face` to slot 5 in the SD layout settings while Gadget is closed. Relaunch Gadget and open slot 5. The face draws a label. The service publishes once per second and appears on the System face. [Workspace settings](DEVELOPMENT.md#workspace-settings)

The bundled Hello Gadget face handles Enter and listens to events from Pulse Service. Both examples can also be built separately.

```bash
python3 -B tools/build_module.py -o build/hello_gadget/module.elf examples/hello_gadget/module.c
cp examples/hello_gadget/module.json build/hello_gadget/
python3 -B tools/build_module.py -o build/pulse_service/module.elf examples/pulse_service/module.c
cp examples/pulse_service/module.json build/pulse_service/
```

Replace the corresponding installed example folders when testing rebuilt copies so each manifest ID is unique. The main package tool builds the five bundled modules and both examples. Clock, Radio, Eyes Mood and Weather show adaptations of existing apps. SDK v1 uses little-endian RV32 ILP32F modules, each with a file and load range up to 2 MiB. Keep modules self-contained and use the Gadget host table for platform calls. Native modules share the firmware's address space and privileges. Manifest capabilities gate the operations exposed through the SDK. [Reference sources](REFERENCE_SOURCES.md) · [BadgeELF loader source](https://github.com/badgeteam/esp32-component-badge-elf/tree/ff5a3c403b7bdb5df3356d2f6f6c3611720f404a)

## Lifecycle and callbacks

- `init` runs once on the main runtime task. `shutdown` releases resources after normal exit or a failed `init`.
- `activate` and `deactivate` follow workspace focus. `input` receives normalized navigation and UTF-8 text events while the face is focused. Gadget handles its global workspace shortcuts and the double `Esc` exit gesture.
- The focused face can handle bare `F1`–`F6` and `Tab` navigation events. The Hyper chords and the two-second Hyper latch still select workspaces.
- Include `gadget_keycap.h` and pass the same `GADGET_KEY_F*` value to `gadget_draw_function_key` as the input binding. The helper draws the matching color and shape using `canvas->rect`. Use `GADGET_KEYCAP_OPAQUE` for an available action and `GADGET_KEYCAP_DIMMED` for an unavailable one. Rebuild the module ELF to pick up the new legends.
- `update` runs on the active face at the manifest's requested interval. Call `invalidate` to request a redraw. `render` borrows a clipped canvas with panel-relative coordinates and rectangle and text drawing callbacks. Use `canvas->width` and `canvas->height` for layout. Canvas handles are valid until `render` returns.
- `service_start` runs after initialization. `worker` is optional and runs on a separate task regardless of workspace focus. A worker blocks or paces itself through `wait_for_stop`. Gadget signals workers and joins them before `service_stop` and `shutdown`.
- `publish` copies the sender ID, topic and integer value into a bounded queue. It returns `false` if the queue is full or the sender is stopping. `event` callbacks run on the runtime task, including for inactive faces. Store data there and let Gadget redraw the active face.

Keep callbacks on the runtime task short. A combined module that shares mutable state between `worker` and runtime callbacks can publish snapshots through the event queue, as Pulse Service does for Hello Gadget.

## Shared services

| Capability | SDK operations |
|---|---|
| Time | `uptime_ms` provides monotonic time. `unix_seconds` uses Tanmatsu RTC time when available and returns `-1` when unavailable. |
| Network | A service worker can open one HTTP(S) GET stream through Gadget. The first request starts shared Wi-Fi on demand using the stored Tanmatsu profiles. Check the HTTP status, read the bytes, then close the stream. |
| Audio | A service worker can exclusively acquire the shared PCM output, write signed interleaved stereo samples and release it. SDK v1 accepts rates from 8 kHz to 48 kHz. |
| Motion | A focused face can read the latest gyro and accelerometer sample. Gadget owns sensor startup and shutdown. |

Gadget releases an open HTTP stream and PCM lease after joining the module worker.

Gadget 0.3.0 appends optional functions to the SDK v1 host table. Check availability with `GADGET_HOST_HAS(host, member)`.

- `civil_time` converts valid RTC time to the launcher's local time zone or UTC. `setting_string` returns a borrowed manifest string valid for the module lifetime. `format` provides bounded printf-style text formatting.
- `wake_worker` interrupts `wait_for_stop` so a worker can pick up a new request. `copy_shared` serializes copies of up to `GADGET_SHARED_COPY_LIMIT` bytes. Use it for every overlapping access to a module-owned record shared between the runtime task and a worker.
- `network_connected` checks the shared Wi-Fi connection from a service worker. `http_read_timed` takes a 1–1500 ms timeout and returns `GADGET_HTTP_AGAIN` if the timeout expires without bytes. Close a stream after EOF or a transport error.
- The `json` table parses a NUL-terminated document of up to 8 KiB. Its member, array and string results are borrowed from the document until `destroy`.
- A runtime event has an empty sender. `GADGET_EVENT_WEATHER_REFRESH` signals `Hyper` + `R` to the Weather service regardless of focus.

Gadget 0.3.1 adds optional worker registration, buffered PCM, motion and region rendering to the same host table.

- `add_worker` registers a task during `service_start`. A module can have up to three workers including its original `worker` callback. Each worker has an independent HTTP stream slot. `wake_worker` notifies all workers, and Gadget joins them before unloading the image.
- `audio_open_buffered` starts a prebuffered stereo PCM output at a selected hardware volume. `audio_write` returns the number of complete frames queued and returns zero when the queue is full. Retry remaining frames on the same worker. `audio_volume` sets the codec volume and `audio_playing` reports whether the queue has started playing.
- `motion_read` supplies a recent timestamped sample to a focused face declaring `motion`. Check each channel's ready flag.
- `invalidate_region` schedules a workspace-relative rectangle. When this host entry is available, the canvas also supplies an opaque `ellipse` primitive and the current damage clip. A zero-width damage value denotes a full redraw.
- `dns_query` lets a network worker exchange a bounded DNS datagram with the configured resolver. The directory module uses it for Radio Browser SRV discovery.

Gadget 0.3.2 adds SD state, hardware volume events and text measurement to SDK v1.

- Modules declaring `storage` can read and write integers and strings through `state_int`, `state_string`, `state_set_int` and `state_set_string` on the runtime task. String results are borrowed until the next set. Gadget stores version 1 JSON in `/sd/data/<slug>/<module-id>/settings.json`. The setter returns success when it queues a write. On startup Gadget uses the first valid current, pending or backup settings copy. Use the integer fallback or handle a `NULL` string to supply a module default.
- For a module-owned text file up to 64 KiB, use `state_read_file` during `init` and `state_write_file` from the runtime task. Reads return the byte count or a `GADGET_STATE_FILE_*` error. Allow one byte for the NUL terminator. A module can try the pending and backup copies when the current file is missing or invalid. Gadget copies each queued snapshot, combines pending writes to the same file, and flushes the queue on clean exit. `settings.json` and the `.tmp` and `.bak` suffixes are reserved.
- `GADGET_EVENT_VOLUME_STEP` carries `+1` or `-1` from the physical volume buttons. `GADGET_EVENT_AUDIO_JACK` reports whether headphones are plugged in. Both runtime events have an empty sender. A PCM lease owner can set separate speaker and headphone volume with `audio_levels`. Use `audio_headphones` to identify the current route.
- The canvas `text_bounds` callback measures the ink rectangle and advance of a single line using the same bitmap font as `text`. Its offsets are relative to the draw origin. It returns false for empty text, unsupported glyphs or invalid input.

Gadget 0.4.0 appends `system_status` and `system_service` to the SDK v1 host table. Check both with `GADGET_HOST_HAS` and call them from runtime callbacks. `system_status` copies the latest heap, CPU, stack and transition measurements, current Wi-Fi and PCM states, and loaded module and service counts. `cpu_count` becomes available after a second sample. `system_service` takes a zero-based service index and copies the loaded service's name and registered and running worker counts. A worker waiting for work counts as running until its entry callback returns.

Gadget 0.5.1 adds optional checked file writes and ICY requests to SDK v1.

- Use the optional `state_write_file_checked` and `state_write_status` callbacks to show the result of an explicit SD file write. Check both with `GADGET_HOST_HAS`. The runtime copies the supplied bytes and accepts one checked write at a time per module. The status reports pending, saved, or failed. The filesystem write runs on the storage worker and is flushed on clean exit.
- `http_open_icy` is an optional alternative to `http_open` for a network worker that can handle ICY framing. The call requests stream metadata and returns the number of audio bytes between metadata blocks. An interval of zero indicates unframed audio.

Gadget 0.6.0 appends the optional `battery_status` callback. Check it with `GADGET_HOST_HAS` and call it from a runtime callback. A successful call returns the cached battery presence, estimated charge percentage, charging state and external power state. The runtime reads the battery about every 15 seconds.

SD revision 12 adds two optional SDK v1 entries. `system_details` copies heap capacities and the cached Wi-Fi identity, addresses and signal strength. The focused module receives `GADGET_EVENT_SYSTEM_SAMPLE` after the shared five-second sample. The `aac` table decodes complete ADTS frames to 16-bit PCM. Open, decode and close each handle on the same worker. Check both entries with `GADGET_HOST_HAS`.
