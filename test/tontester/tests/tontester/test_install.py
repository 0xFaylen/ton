import os
from pathlib import Path

from tontester.install import Install, _executable_name


def test_executable_name_matches_host_platform():
    expected_suffix = ".exe" if os.name == "nt" else ""
    assert _executable_name("validator-engine/validator-engine") == (
        "validator-engine/validator-engine" + expected_suffix
    )


def test_install_executable_paths_match_host_platform(tmp_path: Path):
    install = Install(tmp_path / "build", tmp_path / "source")
    expected_suffix = ".exe" if os.name == "nt" else ""

    assert install.fift_exe.name == "create-state" + expected_suffix
    assert install.key_helper_exe.name == "generate-random-id" + expected_suffix
    assert install.validator_engine_exe.name == "validator-engine" + expected_suffix
    assert install.dht_server_exe.name == "dht-server" + expected_suffix
    assert (
        install.validator_engine_console_exe.name
        == "validator-engine-console" + expected_suffix
    )
    assert install.blockchain_explorer_exe.name == "blockchain-explorer" + expected_suffix
