import subprocess
import sys
from pathlib import Path

from hatchling.builders.hooks.plugin.interface import BuildHookInterface


class CustomBuildHook(BuildHookInterface):
    def initialize(self, version, build_data):
        # Skip during editable installs; CI compiles separately via scripts/compile_c.py
        if version == "editable":
            return
        root = Path(__file__).resolve().parent
        subprocess.run(
            [sys.executable, str(root / "scripts" / "compile_c.py"), "--force"],
            cwd=root,
            check=True,
        )
