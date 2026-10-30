"""Validate the generated LeviLamina client package, not just C++ compilation."""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "bin" / "BedrockSceneCapture"
MANIFEST = PACKAGE / "manifest.json"

def verify() -> None:
    if not MANIFEST.is_file():
        raise SystemExit(f"Missing packaged manifest: {MANIFEST}")
    raw = MANIFEST.read_text(encoding="utf-8")
    if "${" in raw:
        raise SystemExit("Unexpanded modpacker variable in packaged manifest")
    data = json.loads(raw)
    expected = {
        "name": "BedrockSceneCapture",
        "type": "native",
        "platform": "client",
        "entry": "BedrockSceneCapture.dll",
        "version": json.loads((ROOT / "tooth.json").read_text(encoding="utf-8"))["version"],
    }
    for field, value in expected.items():
        if data.get(field) != value:
            raise SystemExit(
                f"Invalid packaged manifest {field}: {data.get(field)!r}, expected {value!r}"
            )
    dll = PACKAGE / expected["entry"]
    if not dll.is_file() or dll.stat().st_size == 0:
        raise SystemExit(f"Missing or empty packaged DLL: {dll}")
    # The loader rejects a native DLL unless it exports the symbol installed by
    # ll/api/memory/MemoryOperators.h. Check the final PE export table, not source.
    symbols = subprocess.run(
        ["llvm-readobj", "--coff-exports", str(dll)],
        check=True, capture_output=True, text=True,
    ).stdout
    if "Name: ll_memory_operator_overrided" not in symbols:
        raise SystemExit("DLL is missing LeviLamina's unified memory operator export")
    print(f"Verified LeviLamina client package and memory operators: {PACKAGE} ({dll.stat().st_size} DLL bytes)")

if __name__ == "__main__":
    verify()
