# Gadget

Gadget 0.6.0 is an ESP32-P4 runtime for Tanmatsu, packaged as SD revision 14. Gadget Dice switches among six full-screen workspaces on a vertical reel. Services such as audio playback keep running across workspace changes. [Revision 14 validation](docs/DEVELOPMENT.md#gadget-060-sd-revision-14-validation)

Gadget packages install only to the SD card. Feature modules ship as separate loadable packages inside Gadget's SD app directory. [SD-card packaging](docs/DEVELOPMENT.md#sd-card-packaging)

## Build the SD package

```bash
./tools/idf.sh --version
./tools/idf.sh build
python3 -B tools/package_sd.py
```

Run these commands from the source checkout. The build uses ESP-IDF 6.0.2, Badge BSP 2.0.0 and PAX Graphics 2.2.2. `sdkconfig.defaults` and `dependencies.lock` carry the build settings and dependency pins. The packager stages a development install under `build/sd/apps/local.gadget/`. [Build environment](docs/DEVELOPMENT.md#build-environment)

## Develop a module

Use `tools/new_module.py` to create a face or service. Build its ELF against the public SDK header and copy the module folder into Gadget's `modules/` directory on SD. [SDK quick start](docs/MODULE_SDK.md#build-and-install)

## Workspace controls

Hold `Hyper` and press one of Tanmatsu's colored symbol keys to switch faces. For one-handed use, tap and release `Hyper`, then press a symbol key within two seconds. An intervening keypress cancels the shortcut.

The startup guide lists the faces loaded from SD and their current shortcuts. When **Press Space to continue** appears, press `Space` to open the first reel slot.

Press `Hyper` + `H` to reopen the guide and `Enter` to return to the current face. Two distinct `Esc` presses within 850 ms return to the Tanmatsu launcher. Tap the power button to turn off the screen and keyboard backlight while services continue. Tap it again to resume the current screen. [Bundled module controls](docs/DEVELOPMENT.md#workspace-controls-and-regression-checks)

Press the red cross in the help guide to arrange faces and remap their shortcuts. Press the green circle to save, then relaunch Gadget to apply the layout. The default slots are Clock, Radio, Eyes, Weather, Hello Gadget and System in that order. `Hyper` + the purple diamond opens System for battery charge, heap, CPU, task stack, reel timing and service status in the default layout. [Workspace settings](docs/DEVELOPMENT.md#workspace-settings)

System refreshes CPU, heap and Wi-Fi information every five seconds. The overview shows the connected network and places signal bars beside its Wi-Fi status. Press `Tab` for signal strength in dBm, the Wi-Fi MAC and assigned IP addresses. Press `Tab` again for the overview.

## Internet Radio

Open Radio with `Hyper` + the orange triangle in the default layout. Use the arrows to highlight a station and `Enter` to play it. The red cross opens Options, where you can add a direct MP3 or AAC stream and change the theme. Use the orange triangle to find stations by name or country. Search looks ahead for playable streams, shows the reported codec and bitrate when available, and marks HLS streams as **Playback unavailable**. Press the purple diamond on a search result or `Tab` in Now Playing to see its stream URL and format details. Use Backspace to remove the highlighted station. Radio saves station changes on the SD card. [Radio controls and Tanmatsu validation](docs/DEVELOPMENT.md#internet-radio)

## Project documentation

- [Implementation milestones](docs/MILESTONES.md) tracks Gadget's features and validation.
- [Development and SD-card installation](docs/DEVELOPMENT.md) covers the toolchain, version pins, SD package and device validation.
- [Architecture](docs/ARCHITECTURE.md) describes the runtime and module SDK.
- [Module SDK v1](docs/MODULE_SDK.md) covers the manifest, callbacks, services and module build command.
- [Reference sources](docs/REFERENCE_SOURCES.md) records the source revisions and integration opportunities for Chronos, TanRadio, Eyes Mood and Weather.
- [Contributing](CONTRIBUTING.md) covers source, module and release changes.

Gadget uses the MIT license. The consolidated license includes terms for bundled code and assets. [LICENSE](LICENSE)
