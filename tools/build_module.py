#!/usr/bin/env python3
"""Build a Gadget SDK v1 native module for the ESP32-P4."""

import argparse
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "components/gadget_sdk/include"


def build_module(compiler: str, sources: list[Path], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = ROOT / ".cache/tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    sources = [source.resolve() for source in sources]
    source_dirs = sorted({ROOT, *(source.parent for source in sources)})
    subprocess.run([
        compiler, "-std=c17", "-Os", "-Wall", "-Wextra", "-Werror", "-fPIC", "-ffreestanding",
        "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
        "-fvisibility=hidden", "-ffunction-sections", "-fdata-sections",
        "-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f", "-msmall-data-limit=0", "-mno-relax",
        *(f"-ffile-prefix-map={path}=." for path in source_dirs),
        "-I", str(SDK), "-shared", "-nostdlib",
        "-Wl,--no-undefined,-Bsymbolic,--entry=gadget_module_entry,--no-relax,--gc-sections",
        "-o", str(output), *map(str, sources), str(SDK.parent / "module_memory.c"),
    ], env={**os.environ, "TMPDIR": str(temporary)}, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="riscv32-esp-elf-gcc", help="ESP32-P4 C compiler")
    parser.add_argument("-o", "--output", type=Path, required=True, help="module ELF to write")
    parser.add_argument("sources", type=Path, nargs="+")
    args = parser.parse_args()
    build_module(args.cc, args.sources, args.output)
