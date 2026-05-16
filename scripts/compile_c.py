#!/usr/bin/env python3
# this_file: scripts/compile_c.py
"""
Standalone C library compilation script.
Handles cross-platform compilation of the audio-stretch library.
"""

import argparse
import sys
from pathlib import Path


def main():
    """Main compilation script."""
    parser = argparse.ArgumentParser(description="Compile audio-stretch C library")
    parser.add_argument("--force", action="store_true", help="Force recompilation")
    parser.add_argument("--clean", action="store_true", help="Clean compiled libraries")
    parser.add_argument("--source-dir", type=Path, help="Audio-stretch source directory")
    parser.add_argument("--output-dir", type=Path, help="Output directory for libraries")
    parser.add_argument("--verbose", action="store_true", help="Verbose output")
    
    args = parser.parse_args()
    
    # Load build module directly to avoid triggering audiostretchy.__init__
    # (which imports pedalboard's native extension and can SIGILL on some platforms)
    import importlib.util
    project_root = Path(__file__).parent.parent
    _build_py = project_root / "src" / "audiostretchy" / "c_interface" / "build.py"
    _spec = importlib.util.spec_from_file_location("_audiostretchy_c_build", _build_py)
    _mod = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_mod)
    AudioStretchBuilder = _mod.AudioStretchBuilder
    
    # Create builder
    builder = AudioStretchBuilder(args.source_dir, args.output_dir)
    
    if args.verbose:
        print(f"Source directory: {builder.source_dir}")
        print(f"Output directory: {builder.output_dir}")
        print(f"Platform: {builder.system} ({builder.arch})")
    
    # Perform requested action
    if args.clean:
        builder.clean()
    else:
        lib_path = builder.compile_library(force=args.force)
        print(f"Compiled library: {lib_path}")


if __name__ == "__main__":
    main()