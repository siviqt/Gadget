# Gadget

Gadget 0.5.0 is an ESP32-P4 runtime for Tanmatsu, packaged as SD revision 8. Gadget Dice switches among six full-screen workspaces on a vertical reel. Services such as audio playback keep running across workspace changes. [Release checks](docs/DEVELOPMENT.md#m5-release-validation)

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

Hold `Hyper` and press `F1`–`F6` to switch faces. For one-handed use, tap and release `Hyper`, then press one of those function keys within two seconds. An intervening keypress cancels the shortcut.

The startup guide lists the faces loaded from SD and their current shortcuts. Gadget opens the first reel slot once the modules are ready and the guide has been visible for about three seconds. Press `Enter` to continue as soon as the modules are ready, or choose a face with its shortcut.

Press `Hyper` + `H` to reopen the guide and `Enter` to return to the current face. Two distinct `Esc` presses within 850 ms return to the Tanmatsu launcher. [Bundled module controls](docs/DEVELOPMENT.md#workspace-controls-and-regression-checks)

Press `C` in the guide to arrange faces and remap `F1`–`F6`. Press `Enter` to save, then relaunch Gadget to apply the layout. The default slots are Clock, Radio, Eyes, Weather, Hello Gadget and System in that order. `Hyper` + `F6` opens System for heap, CPU, task stack, reel timing and service status in the default layout. [Workspace settings](docs/DEVELOPMENT.md#workspace-settings)

## Project documentation

- [Implementation milestones](docs/MILESTONES.md) tracks Gadget's features and validation.
- [Development and SD-card installation](docs/DEVELOPMENT.md) covers the toolchain, version pins, SD package and device validation.
- [Architecture](docs/ARCHITECTURE.md) describes the runtime and module SDK.
- [Module SDK v1](docs/MODULE_SDK.md) covers the manifest, callbacks, services and module build command.
- [Reference sources](docs/REFERENCE_SOURCES.md) records the source revisions and integration opportunities for Chronos, TanRadio, Eyes Mood and Weather.
- [Contributing](CONTRIBUTING.md) covers source, module and release changes.

Gadget uses the MIT license. The consolidated license includes terms for bundled code and assets. [LICENSE](LICENSE)
