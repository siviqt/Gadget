#!/usr/bin/env python3
"""Stage a built Gadget app as a Tanmatsu SD package."""

import argparse
import fcntl
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import tempfile
from urllib.parse import urlsplit
import zlib

from build_module import build_module


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SLUG = "local.gadget"
BUNDLED_MODULES = (
    ROOT / "modules/clock",
    ROOT / "modules/internet_radio",
    ROOT / "modules/eyes_mood",
    ROOT / "modules/weather",
    ROOT / "modules/system_status",
    ROOT / "examples/hello_gadget",
    ROOT / "examples/pulse_service",
)


def png_chunk(name: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))


def cube_icon(size: int) -> bytes:
    # Points use a clockwise, 64-by-64 design grid.
    faces = (
        (((32, 8), (53, 20), (32, 32), (11, 20)), (128, 226, 245)),
        (((11, 22), (31, 34), (31, 56), (11, 44)), (49, 150, 182)),
        (((33, 34), (53, 22), (53, 44), (33, 56)), (30, 101, 140)),
    )
    rows = bytearray()
    for y in range(size):
        rows.append(0)  # PNG filter: none
        py = (y + 0.5) * 64 / size
        for x in range(size):
            px = (x + 0.5) * 64 / size
            color = (17, 25, 35)
            for points, face_color in faces:
                edges = zip(points, (*points[1:], points[0]))
                if all((bx - ax) * (py - ay) >= (by - ay) * (px - ax) for (ax, ay), (bx, by) in edges):
                    color = face_color
                    break
            rows.extend(color)
    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib.compress(rows)) + png_chunk(b"IEND", b"")


def package_license(project: dict) -> str:
    idf = Path(project["idf_path"])
    managed = ROOT / "managed_components"
    config = Path(project["config_file"]).read_text().splitlines()
    unused_libc_license = "COPYING.NEWLIB" if "CONFIG_LIBC_PICOLIBC=y" in config else "COPYING.picolibc"
    unused_libc_notice = idf / "components/esp_libc" / unused_libc_license
    notices = {"ESP-IDF/LICENSE": idf / "LICENSE"}
    for component in map(Path, project["build_component_paths"]):
        if not component.is_relative_to(managed) and not component.is_relative_to(idf / "components"):
            continue
        for file in component.rglob("*"):
            if file == unused_libc_notice:
                continue
            if re.match(r"^(?:LICEN[CS]E|NOTICE|COPYING|COPYRIGHT)(?:[._-]|$)", file.name, re.I) and file.is_file():
                label = f"{component.name}/{file.relative_to(component)}"
                notices[label] = file
    credits = {}
    for label, path in sorted(notices.items()):
        credits.setdefault(path.read_text(errors="replace").strip(), []).append(label)
    sections = [(ROOT / "LICENSE").read_text().rstrip(), "Firmware dependency notices\n==========================="]
    for notice, labels in credits.items():
        sections.append("\n".join(labels) + "\n\n" + notice)
    return "\n\n".join(sections) + "\n"


def release_metadata(metadata: dict, path: Path, parser: argparse.ArgumentParser) -> dict:
    try:
        release = json.loads(path.read_text())
    except (OSError, ValueError):
        parser.error("unable to read the release metadata overrides")
    if (not isinstance(release, dict) or set(release) != {"author", "repository_url"} or
            not all(isinstance(value, str) and value.strip() for value in release.values())):
        parser.error("release overrides require author and repository_url")
    try:
        url = urlsplit(release["repository_url"])
    except ValueError:
        parser.error("repository_url must be a public HTTPS URL")
    if url.scheme != "https" or not url.hostname or url.username is not None or url.query or url.fragment:
        parser.error("repository_url must be a public HTTPS URL")
    metadata["author"] = release["author"]
    metadata.pop("repository_url", None)
    return {
        "name": metadata.pop("name"),
        "description": metadata.pop("description"),
        "repository_url": release["repository_url"],
        **metadata,
    }


def stage_package(package: Path, build: Path, project: dict, metadata: dict, target: dict) -> None:
    shutil.copyfile(build / target["executable"], package / target["executable"])
    (package / "LICENSE").write_text(package_license(project))
    for dimensions, name in metadata["icon"].items():
        (package / name).write_bytes(cube_icon(int(dimensions.split("x", 1)[0])))
    for source in BUNDLED_MODULES:
        module = package / "modules" / source.name
        build_module(project["c_compiler"], sorted(source.glob("*.c")), module / "module.elf")
        shutil.copyfile(source / "module.json", module / "module.json")
    (package / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build", help="ESP-IDF output directory under Gadget")
    parser.add_argument("--slug", default=DEFAULT_SLUG, help="SD app folder and AppFS cache name")
    parser.add_argument("--release-metadata", type=Path, help="private JSON with the release author and repository URL")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,47}", args.slug) or args.slug.endswith("."):
        parser.error("use an AppFS slug of up to 48 letters, digits, dots, underscores or hyphens")
    build = (ROOT / args.build_dir).resolve()
    if not build.is_relative_to(ROOT):
        parser.error("the build directory must be inside Gadget")

    metadata = json.loads((ROOT / "metadata.json").read_text())
    target = next(app for app in metadata["application"] if "tanmatsu" in app["targets"])
    if target["type"] != "appfs" or target.get("external_only") is not True or target.get("internal_only"):
        parser.error("the Tanmatsu package must require an SD install")
    if args.release_metadata:
        metadata = release_metadata(metadata, args.release_metadata, parser)

    filenames = [target["executable"], *metadata["icon"].values()]
    if (metadata["license_type"] != "MIT" or metadata["license_file"] != "LICENSE" or
            any(not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", name) for name in filenames) or
            len(set(filenames + ["metadata.json", "LICENSE"])) != len(filenames) + 2):
        parser.error("metadata must name distinct root files and the consolidated MIT LICENSE")
    project = json.loads((build / "project_description.json").read_text())
    if project["project_version"] != metadata["version"] or project["target"] != "esp32p4":
        parser.error("rebuild the ESP32-P4 app with the version from metadata.json")

    package = build / "sd" / "apps" / args.slug
    package.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".gadget-", dir=package.parent) as temporary:
        staged = Path(temporary) / "package"
        staged.mkdir()
        stage_package(staged, build, project, metadata, target)
        files = {file.relative_to(staged).as_posix() for file in staged.rglob("*") if file.is_file()}
        sources = [asset["source_file"] for asset in target["assets"]]
        targets = [PurePosixPath(asset["target_file"]) for asset in target["assets"]]
        if (set(sources) != files.difference(filenames, {"metadata.json"}) or
                len(sources) != len(set(sources)) or len(targets) != len(set(targets)) or
                any(path == PurePosixPath(".") or path.is_absolute() or ".." in path.parts for path in targets) or
                targets[sources.index("LICENSE")] != PurePosixPath("LICENSE")):
            parser.error("assets must declare each package file and install under the app directory")

        with (build / ".package_sd.lock").open("a") as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            previous = Path(temporary) / "previous"
            if package.is_symlink() or (package.exists() and not package.is_dir()):
                parser.error("the package path must be a directory")
            if package.exists():
                package.rename(previous)
            try:
                staged.rename(package)
            except OSError:
                if previous.exists():
                    previous.rename(package)
                raise
    print(package.relative_to(ROOT))


if __name__ == "__main__":
    main()
