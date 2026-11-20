# Reference sources

The project and revision identify the material used to develop each module. Keep the original reference trees read-only. [Contributing](../CONTRIBUTING.md)

| Gadget module | Upstream project and inspected revision | Adaptation starting point |
|---|---|---|
| Clock | Chronos at `fb9d6e002b6b1bb13057685ffb1e9f432c21039b` | Inspect `components/chronos_app/features/clock.c` for RTC, system time and time-zone handling. Adapt the date and calendar presentation through the Gadget surface. |
| Internet Radio | TanRadio at `53a5465be4f1a375495e71c33737f93c8b99ff83` | Review `main/radio/`, `main/audio/` and `main/network/` for bounded buffering, decoding, reconnection and audio output. Put playback state and workers behind the shared service. |
| Eyes Mood | Eyes-plugin at `14d33d2269fe862957683c82b17eaf028170bcc4` | Review `src/eyes_engine.c` for animation and moods and `src/widget_display.c` for drawing. Use SDK events and a Gadget surface for animation and input. |
| Weather | Weather at `8f78b847f369a8d633e6117ad9ee0e00d4710e10` | Review `src/weather.c`, `src/json.c` and `src/config.c` for provider requests, parsing and settings. Refresh through shared network access and hand a cached snapshot to the face. |

## M3 adaptations

| Gadget module | Adopted behavior | Notices |
|---|---|---|
| [Clock](../modules/clock/) | Chronos civil date arithmetic and RTC-to-local-time approach. Gadget reads the launcher's saved POSIX time zone. | Chronos MIT |
| [Internet Radio](../modules/internet_radio/) | TanRadio's 256 KiB compressed and PCM buffers, output routing, volume button repeats, reconnect, station search and SD station format. The presets use SomaFM's direct MP3 streams for personal listening. | TanRadio MIT, [Radio Browser API](https://api.radio-browser.info/), [SomaFM direct links](https://somafm.com/groovesalad/directstreamlinks.html) |
| [Eyes Mood](../modules/eyes_mood/) | Eyes-plugin gyro direction mapping, tilt response, gaze and lid targets, blinking and elliptical eye drawing, adapted to the Gadget IMU service and canvas. | Eyes-plugin MIT |
| [Weather](../modules/weather/) | Weather's Open-Meteo geocoding, request fields and metric validation with three days of forecast. Geocoding uses data from GeoNames. | Weather MIT, [Open-Meteo terms and attribution](https://open-meteo.com/en/terms), [Open-Meteo geocoding](https://open-meteo.com/en/docs/geocoding-api) |

Radio embeds the scalar Layer III decoder from minimp3 revision `142823796ab8efa55e1bf11406028f3be77ecceb`. The source and SD distributions collect the reference licenses and the decoder's CC0 notice in `LICENSE`. [Consolidated license](../LICENSE) · [Pinned minimp3 source](https://github.com/lieff/minimp3/tree/142823796ab8efa55e1bf11406028f3be77ecceb)

Gadget builds five PAX font assets from `modules/ui_fonts/`. The 20 px medium, 24 px semibold and 56 px monospaced semibold assets come from the Chronos revision above. Gadget regenerated the 14 px and 18 px IBM Plex Sans Regular assets from IBM Plex `78cd4223d8de9fcb78cba84eadecb269c56093c5` using the Chronos generator with Pillow 12.3.0 and fontTools 4.61.1. [Active font sources](../components/gadget_graphics/CMakeLists.txt)

To regenerate the two regular assets, work in a Gadget-owned copy of the pinned Chronos `tools/generate_fonts.py` and `assets/fonts/`. Select Sans Regular at 14 and 18 px with `ui` coverage and add the characters in U+0400–U+04FF present in the IBM Plex Sans Regular cmap. The original Chronos coverage and compatibility glyph definitions provide the other characters. Both output files have 919 glyphs. The consolidated license includes the IBM Plex and Chakra Petch compatibility licenses. [Consolidated license](../LICENSE)

The Dice uses Gadget's RGB565 renderer and PAX. SynthEngine3D was evaluated as a reference for the original cube transition. [Inspected SynthEngine3D revision](https://github.com/nullislandspace/synthengine3D/tree/061c26bb37a6d508653bd6df09ff3f04ac65a071)
