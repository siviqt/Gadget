# Contributing to Gadget

## Build and test

Use the pinned ESP-IDF toolchain and build from the Gadget checkout.

```bash
./tools/idf.sh build
python3 -B tools/package_sd.py
```

Keep `sdkconfig.defaults`, `main/idf_component.yml` and `dependencies.lock` together. For firmware or packaging changes, build from a fresh source copy and exercise the install, launch, update and exit checks on a Tanmatsu. Record firmware and hardware revisions alongside reproducible results. [Release checks](docs/DEVELOPMENT.md#m5-release-validation)

## Work on modules

Start with the public SDK and the face or service scaffold. Keep app-specific code and assets in a module folder and shared runtime facilities in `components/`. Check module startup, focus changes and cleanup after an exit. For a service, also check activity while another workspace is focused. [Module SDK](docs/MODULE_SDK.md)

Keep reference checkouts read-only. Record the revision and adapted behavior for upstream work and add its notice to the consolidated license. [Reference sources](docs/REFERENCE_SOURCES.md) · [LICENSE](LICENSE)

## Prepare a release

Share source, synthetic test inputs and relevant diagnostic excerpts. Keep local paths, saved settings, network credentials, device identifiers and private build logs in local storage. Supply the public author and repository link through the release metadata input when preparing the Tanmatsu repository entry. [SD-card packaging](docs/DEVELOPMENT.md#sd-card-packaging)
