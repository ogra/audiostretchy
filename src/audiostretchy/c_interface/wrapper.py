# this_file: src/audiostretchy/c_interface/wrapper.py
"""
Python ctypes wrapper for the audio-stretch C library.
Provides high-level interface to the TDHS (Time-Domain Harmonic Scaling) algorithm.
"""

import ctypes
import platform
from pathlib import Path
from typing import Optional

import numpy as np


class TDHSAudioStretch:
    """
    Python wrapper for the audio-stretch C library using TDHS algorithm.
    Provides time-stretching capabilities without pitch modification.
    """

    STRETCH_FAST_FLAG = 0x1
    STRETCH_DUAL_FLAG = 0x2

    def __init__(
        self, 
        shortest_period: int, 
        longest_period: int, 
        num_chans: int, 
        flags: int
    ) -> None:
        """
        Initialize the stretching context.

        Args:
            shortest_period: Minimum period length for pitch detection
            longest_period: Maximum period length for pitch detection
            num_chans: Number of audio channels (1 or 2)
            flags: Algorithm behavior flags (STRETCH_FAST_FLAG, STRETCH_DUAL_FLAG)
        """
        self._lib = self._load_library()
        self._setup_function_signatures()
        
        self.handle = self.stretch_init(shortest_period, longest_period, num_chans, flags)
        if not self.handle:
            raise RuntimeError("Failed to initialize audio stretch context")

    def _load_library(self) -> ctypes.CDLL:
        """Load the appropriate shared library for the current platform."""
        system = platform.system()
        arch = platform.machine().lower()
        
        # Determine library filename based on platform and architecture
        if system == "Windows":
            if arch in ("amd64", "x86_64"):
                lib_name = "_stretch_x64.dll"
            else:
                lib_name = "_stretch.dll"
        elif system == "Darwin":  # macOS
            if arch in ("arm64", "aarch64"):
                lib_name = "_stretch_arm64.dylib"
            else:
                lib_name = "_stretch_x64.dylib"
        elif system == "Linux":
            if arch in ("aarch64", "arm64"):
                lib_name = "_stretch_aarch64.so"
            else:
                lib_name = "_stretch_x64.so"
        else:
            raise RuntimeError(f"Unsupported platform: {system}")

        # Look for the library in the package directory
        lib_path = Path(__file__).parent / "lib" / lib_name
        
        if not lib_path.exists():
            # Fallback to generic name
            generic_names = {
                "Windows": "_stretch.dll",
                "Darwin": "_stretch.dylib", 
                "Linux": "_stretch.so"
            }
            lib_path = Path(__file__).parent / "lib" / generic_names[system]
            
        if not lib_path.exists():
            raise RuntimeError(f"Audio stretch library not found at {lib_path}")

        try:
            return ctypes.cdll.LoadLibrary(str(lib_path))
        except OSError as e:
            raise RuntimeError(f"Failed to load audio stretch library: {e}") from e

    def _setup_function_signatures(self) -> None:
        """Set up ctypes function signatures for the C library."""
        # stretch_init
        self.stretch_init = self._lib.stretch_init
        self.stretch_init.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        self.stretch_init.restype = ctypes.c_void_p

        # stretch_output_capacity
        self.stretch_output_capacity = self._lib.stretch_output_capacity
        self.stretch_output_capacity.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_float]
        self.stretch_output_capacity.restype = ctypes.c_int

        # stretch_samples
        self.stretch_samples = self._lib.stretch_samples
        self.stretch_samples.argtypes = [
            ctypes.c_void_p,
            np.ctypeslib.ndpointer(dtype=np.int16),
            ctypes.c_int,
            np.ctypeslib.ndpointer(dtype=np.int16),
            ctypes.c_float,
        ]
        self.stretch_samples.restype = ctypes.c_int

        # stretch_samples_float
        self.stretch_samples_float = self._lib.stretch_samples_float
        self.stretch_samples_float.argtypes = [
            ctypes.c_void_p,
            np.ctypeslib.ndpointer(dtype=np.float32),
            ctypes.c_int,
            np.ctypeslib.ndpointer(dtype=np.float32),
            ctypes.c_float,
        ]
        self.stretch_samples_float.restype = ctypes.c_int

        # stretch_flush
        self.stretch_flush = self._lib.stretch_flush
        self.stretch_flush.argtypes = [
            ctypes.c_void_p,
            np.ctypeslib.ndpointer(dtype=np.int16),
        ]
        self.stretch_flush.restype = ctypes.c_int

        # stretch_flush_float
        self.stretch_flush_float = self._lib.stretch_flush_float
        self.stretch_flush_float.argtypes = [
            ctypes.c_void_p,
            np.ctypeslib.ndpointer(dtype=np.float32),
        ]
        self.stretch_flush_float.restype = ctypes.c_int

        # stretch_reset
        self.stretch_reset = self._lib.stretch_reset
        self.stretch_reset.argtypes = [ctypes.c_void_p]
        self.stretch_reset.restype = None

        # stretch_deinit
        self.stretch_deinit = self._lib.stretch_deinit
        self.stretch_deinit.argtypes = [ctypes.c_void_p]
        self.stretch_deinit.restype = None

    def output_capacity(self, max_num_samples: int, max_ratio: float) -> int:
        """
        Calculate required output buffer capacity.

        Args:
            max_num_samples: Maximum number of input samples
            max_ratio: Maximum stretch ratio expected

        Returns:
            Required output buffer size in samples
        """
        return self.stretch_output_capacity(self.handle, max_num_samples, max_ratio)

    def process_samples(
        self, 
        samples: np.ndarray, 
        num_samples: int, 
        output: np.ndarray, 
        ratio: float
    ) -> int:
        """
        Process audio samples with specified stretch ratio.

        Args:
            samples: Input audio samples (int16)
            num_samples: Number of samples per channel
            output: Output buffer (int16)
            ratio: Stretch ratio (>1.0 = slower, <1.0 = faster)

        Returns:
            Number of output samples produced
        """
        return self.stretch_samples(self.handle, samples, num_samples, output, ratio)

    def process_samples_float(
        self,
        samples: np.ndarray,
        num_samples: int,
        output: np.ndarray,
        ratio: float
    ) -> int:
        """
        Process audio samples with specified stretch ratio.

        Args:
            samples: Input audio samples (float32)
            num_samples: Number of samples per channel
            output: Output buffer (float32)
            ratio: Stretch ratio (>1.0 = slower, <1.0 = faster)

        Returns:
            Number of output samples produced
        """
        return self.stretch_samples_float(
            self.handle, samples, num_samples, output, ratio
        )

    def flush(self, output: np.ndarray) -> int:
        """
        Flush remaining samples from internal buffers.

        Args:
            output: Output buffer (int16)

        Returns:
            Number of flushed samples
        """
        return self.stretch_flush(self.handle, output)

    def flush_float(self, output: np.ndarray) -> int:
        """
        Flush remaining samples from internal buffers.

        Args:
            output: Output buffer (float32)

        Returns:
            Number of flushed samples
        """
        return self.stretch_flush_float(self.handle, output)

    def reset(self) -> None:
        """Reset the stretch context to initial state."""
        self.stretch_reset(self.handle)

    def deinit(self) -> None:
        """Clean up and free the stretch context."""
        if self.handle:
            self.stretch_deinit(self.handle)
            self.handle = None

    def __del__(self) -> None:
        """Ensure cleanup on object destruction."""
        self.deinit()
