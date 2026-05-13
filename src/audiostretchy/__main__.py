#!/usr/bin/env python3
# this_file: src/audiostretchy/__main__.py
"""
Command-line interface for AudioStretchy.
Provides CLI access to audio time-stretching functionality.
"""

import sys

import fire

from .core import stretch_audio


def main():
    """Main CLI entry point."""
    help_requested = any(
        arg == "--help"
        or (arg.startswith("-") and not arg.startswith("--") and "h" in arg)
        for arg in sys.argv[1:]
    )
    fire.core.Display = lambda lines, out: print(
        *lines, file=sys.stdout if help_requested else out
    )
    fire.Fire(stretch_audio)


if __name__ == "__main__":
    main()
