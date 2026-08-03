import os
import subprocess
import sys
from pathlib import Path
from typing import final

from tonlib import TonlibCDLL


def _executable_name(name: str) -> str:
    return f"{name}.exe" if os.name == "nt" else name


@final
class Install:
    def __init__(self, build_dir: Path, source_dir: Path):
        self._build_dir = build_dir.absolute()
        self._source_dir = source_dir.absolute()
        self._tonlibjson = None
        self._dll_directory_handles = []

    @property
    def build_dir(self):
        return self._build_dir

    @property
    def source_dir(self):
        return self._source_dir

    @property
    def fift_exe(self):
        return self.build_dir / _executable_name("crypto/create-state")

    @property
    def fift_include_dirs(self):
        return [
            self.source_dir / "crypto/fift/lib",
            self.source_dir / "crypto/smartcont",
        ]

    @property
    def key_helper_exe(self):
        return self.build_dir / _executable_name("utils/generate-random-id")

    @property
    def validator_engine_exe(self):
        return self.build_dir / _executable_name("validator-engine/validator-engine")

    @property
    def dht_server_exe(self):
        return self.build_dir / _executable_name("dht-server/dht-server")

    @property
    def validator_engine_console_exe(self):
        return self.build_dir / _executable_name(
            "validator-engine-console/validator-engine-console"
        )

    @property
    def blockchain_explorer_exe(self):
        return self.build_dir / _executable_name(
            "blockchain-explorer/blockchain-explorer"
        )

    @property
    def tonlibjson(self):
        if self._tonlibjson is None:
            if sys.platform.startswith("linux"):
                name = "tonlib/libtonlibjson.so"
            elif sys.platform == "darwin":
                name = "tonlib/libtonlibjson.dylib"
            elif sys.platform == "win32":
                name = "tonlib/libtonlibjson.dll"
                for directory in os.environ.get("TON_DLL_DIRS", "").split(os.pathsep):
                    if directory and Path(directory).is_dir():
                        self._dll_directory_handles.append(os.add_dll_directory(directory))
            else:
                raise RuntimeError(f"Unsupported platform: {sys.platform}")
            self._tonlibjson = TonlibCDLL(self.build_dir / name)

        return self._tonlibjson


def run_fift(install: Install, code: str, working_dir: Path):
    script_file = working_dir / "script.fif"
    _ = script_file.write_text(code)

    args = [install.fift_exe]
    for include_dir in install.fift_include_dirs:
        args += ["-I", include_dir]
    args += ["-s", "script.fif"]

    _ = subprocess.run(args, cwd=working_dir, check=True)

    os.remove(script_file)
