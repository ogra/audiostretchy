"""
Backward-compatible TDHS import path.

This module keeps the historical `audiostretchy.interface.tdhs` location,
while delegating the implementation to the unified c_interface wrapper.
"""

from ..c_interface.wrapper import TDHSAudioStretch

__all__ = ["TDHSAudioStretch"]
