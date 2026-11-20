#!/usr/bin/env python3
"""Create a standalone Gadget SDK v1 face or service."""

import argparse
import json
from pathlib import Path
import re


FACE = """\
#include "gadget_module.h"

static void render(gadget_module_context_t *module, const gadget_canvas_t *canvas)
{
    (void)module;
    canvas->text(canvas->handle, 24, 24, 24, UINT32_C(0xfff4f7fb), "New Gadget face");
}

static const gadget_module_t hooks = {
    .abi = GADGET_MODULE_ABI,
    .size = sizeof(gadget_module_t),
    .render = render,
};
"""

SERVICE = """\
#include "gadget_module.h"

static void worker(gadget_module_context_t *module)
{
    while (!module->host->wait_for_stop(module, 1000)) {
        module->host->publish(module, 1, 1);
    }
}

static const gadget_module_t hooks = {
    .abi = GADGET_MODULE_ABI,
    .size = sizeof(gadget_module_t),
    .worker = worker,
};
"""

ENTRY = """
const gadget_module_t *gadget_module_entry(void)
{
    return &hooks;
}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("face", "service"))
    parser.add_argument("id", help="unique module identifier")
    parser.add_argument("directory", type=Path, help="new package directory")
    parser.add_argument("--name", required=True, help="display name as shown on the device")
    parser.add_argument("--slot", type=int, choices=range(1, 7), help="initial face slot (default 5)")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z][a-z0-9._-]{0,30}", args.id):
        parser.error("the module ID must start with a lowercase letter and fit in 31 bytes")
    name = args.name.strip()
    if not name or len(name.encode("utf-8")) > 39:
        parser.error("the display name must contain 1 to 39 UTF-8 bytes")
    if args.kind == "service" and args.slot is not None:
        parser.error("--slot applies to face modules")

    manifest = {
        "format": 1,
        "id": args.id,
        "name": name,
        "version": "0.1.0",
        "sdk": 1,
        "kind": args.kind,
        "executable": "module.elf",
        "capabilities": [],
        "dependencies": [],
    }
    if args.kind == "face":
        manifest["face"] = args.slot or 5
        manifest["startup_preview"] = True
    try:
        args.directory.mkdir(parents=True)
    except FileExistsError:
        parser.error("the package directory already exists")
    (args.directory / "module.json").write_text(json.dumps(manifest, indent=2) + "\n")
    template = FACE if args.kind == "face" else SERVICE
    (args.directory / "module.c").write_text(template + ENTRY)


if __name__ == "__main__":
    main()
