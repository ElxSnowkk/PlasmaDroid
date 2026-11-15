#!/usr/bin/env python3
# This file belongs to the PlasmaDroid project.
#
# Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
#
# PlasmaDroid is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
# or <https://www.gnu.org/licenses/gpl-3.0.html>.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.

"""Embed generated SPIR-V words in a small C++ header for Android's native loader."""
from __future__ import annotations

import pathlib
import struct
import sys

def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit("usage: embed_spirv.py OUTPUT name=path.spv [...]")
    output = pathlib.Path(sys.argv[1])
    entries = sys.argv[2:]
    parts = ["#pragma once", "#include <cstddef>", "#include <cstdint>", "namespace plasma_spv {"]
    for entry in entries:
        name, raw_path = entry.split("=", 1)
        data = pathlib.Path(raw_path).read_bytes()
        if not data or len(data) % 4:
            raise ValueError(f"SPIR-V inválido: {raw_path}")
        words = struct.unpack("<" + "I" * (len(data) // 4), data)
        parts.append(f"inline constexpr uint32_t {name}[] = {{")
        for start in range(0, len(words), 8):
            parts.append("    " + ", ".join(f"0x{word:08x}u" for word in words[start:start + 8]) + ",")
        parts.append("};")
        parts.append(f"inline constexpr size_t {name}Size = sizeof({name});")
    parts.append("} // namespace plasma_spv")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(parts) + "\n", encoding="utf-8")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
