# this_file: src/audiostretchy/core.py
"""
Core AudioStretch class with Pedalboard I/O integration.
Provides high-level interface for audio time-stretching operations.
"""

from io import BytesIO
from pathlib import Path
from typing import BinaryIO, Optional, Union

import numpy as np
from pedalboard.io import AudioFile

from .c_interface import TDHSAudioStretch


class AudioStretch:
    """
    High-level interface for audio time-stretching using TDHS algorithm.
    Uses Pedalboard for audio I/O and the audio-stretch C library for processing.
    """

    def __init__(self):
        """Initialize AudioStretch processor."""
        self.samples: Optional[np.ndarray] = None
        self.samplerate: int = 44100
        self.num_channels: int = 1
        
    def open(
        self,
        path: Optional[Union[str, Path]] = None,
        file: Optional[BinaryIO] = None,
        format: Optional[str] = None,
    ) -> None:
        """
        Open an audio file using Pedalboard.

        Args:
            path: Path to the audio file
            file: Binary I/O object containing audio data
            format: Audio format hint (usually inferred from extension)
        
        Raises:
            ValueError: If neither path nor file is provided
            IOError: If the file cannot be opened or read
        """
        if path is None and file is None:
            raise ValueError("Either path or file must be provided")
            
        input_source = file if file is not None else str(path)
        
        try:
            read_kwargs = {}
            if format is not None:
                read_kwargs["format"] = format
            with AudioFile(input_source, **read_kwargs) as f:
                # Read all audio data into memory
                self.samples = f.read(f.frames)
                self.samplerate = f.samplerate
                self.num_channels = f.num_channels
                
        except Exception as e:
            source_desc = str(path) if path else "file object"
            raise IOError(f"Could not open audio file {source_desc}: {e}") from e

    def save(
        self,
        path: Optional[Union[str, Path]] = None,
        file: Optional[BinaryIO] = None,
        format: Optional[str] = None,
        bit_depth: Optional[int] = None,
    ) -> None:
        """
        Save processed audio using Pedalboard.

        Args:
            path: Path to save the audio file
            file: Binary I/O object to write audio data
            format: Audio format (inferred from path extension if not specified)
            bit_depth: Output bit depth (defaults to 32 for WAV)
            
        Raises:
            ValueError: If no audio data or invalid parameters
            IOError: If the file cannot be written
        """
        if path is None and file is None:
            raise ValueError("Either path or file must be provided")
            
        if self.samples is None:
            raise ValueError("No audio data to save. Call open() and process first")
            
        output_target = file if file is not None else str(path)
        
        # Infer format from path extension if not specified
        if format is None and path is not None:
            format = Path(path).suffix.lstrip(".")

        write_kwargs = {}
        effective_bit_depth = bit_depth
        if effective_bit_depth is None and (format or "").lower() == "wav":
            effective_bit_depth = 32
        if effective_bit_depth is not None:
            write_kwargs["bit_depth"] = effective_bit_depth
            
        try:
            open_kwargs = {
                "mode": "w",
                "samplerate": self.samplerate,
                "num_channels": self.num_channels,
                **write_kwargs,
            }
            if file is not None and format is not None:
                open_kwargs["format"] = format
            with AudioFile(
                output_target,
                **open_kwargs,
            ) as f:
                f.write(self.samples)
                
        except Exception as e:
            target_desc = str(path) if path else "file object"
            raise IOError(f"Could not save audio file to {target_desc}: {e}") from e

    def resample(self, target_samplerate: int) -> None:
        """
        Resample audio to target sample rate using Pedalboard.

        Args:
            target_samplerate: Target sample rate in Hz
            
        Raises:
            ValueError: If no audio data is loaded
        """
        if self.samples is None:
            raise ValueError("No audio data to resample. Call open() first")
            
        if target_samplerate == self.samplerate:
            return  # No resampling needed
            
        self.samples = self._resample_array(self.samples, self.samplerate, target_samplerate)
        self.samplerate = target_samplerate

    @staticmethod
    def _resample_array(
        samples: np.ndarray, source_samplerate: int, target_samplerate: int
    ) -> np.ndarray:
        """Resample channel-first audio with linear interpolation."""
        if source_samplerate == target_samplerate:
            return samples
        num_frames = samples.shape[1]
        target_frames = max(1, int(num_frames * target_samplerate / source_samplerate))
        source_positions = np.arange(num_frames, dtype=np.float64)
        target_positions = np.linspace(0, num_frames - 1, target_frames)
        resampled = np.vstack(
            [
                np.interp(target_positions, source_positions, channel)
                for channel in samples
            ]
        )
        return np.ascontiguousarray(resampled, dtype=np.float32)

    def stretch(
        self,
        ratio: float = 1.0,
        gap_ratio: float = 0.0,
        upper_freq: int = 333,
        lower_freq: int = 55,
        buffer_ms: float = 25.0,
        threshold_gap_db: float = -40.0,
        double_range: bool = False,
        fast_detection: bool = False,
        normal_detection: bool = False,
    ) -> None:
        """
        Stretch audio using the TDHS algorithm.

        Args:
            ratio: Stretch ratio (>1.0 = slower, <1.0 = faster)
            gap_ratio: Separate ratio for silent sections (0.0 = use main ratio)
            upper_freq: Upper frequency limit for period detection (Hz)
            lower_freq: Lower frequency limit for period detection (Hz)
            buffer_ms: Buffer size for silence detection (currently unused)
            threshold_gap_db: Silence threshold in dB (currently unused)
            double_range: Enable extended ratio range (0.25-4.0)
            fast_detection: Use faster period detection algorithm
            normal_detection: Force normal detection (currently unused)
            
        Raises:
            ValueError: If no audio data or invalid parameters
            RuntimeError: If stretching fails
            
        Note:
            gap_ratio, buffer_ms, threshold_gap_db are not currently implemented
            in this Python wrapper and require additional segmentation logic.
        """
        if self.samples is None:
            raise ValueError("No audio data to stretch. Call open() first")
            
        if ratio <= 0:
            raise ValueError("Stretch ratio must be positive")
            
        # Skip processing if no change needed
        effective_gap_ratio = gap_ratio if gap_ratio > 0 else ratio
        if ratio == 1.0 and effective_gap_ratio == 1.0:
            return
            
        # Interleave float32 samples for C library
        samples_float = self._interleave(self.samples)
        
        # Set up TDHS parameters
        min_period = max(1, int(self.samplerate / upper_freq))
        max_period = int(self.samplerate / lower_freq)
        
        flags = 0
        if fast_detection:
            flags |= TDHSAudioStretch.STRETCH_FAST_FLAG
        if double_range or ratio < 0.5 or ratio > 2.0:
            flags |= TDHSAudioStretch.STRETCH_DUAL_FLAG
            
        # Initialize stretcher
        stretcher = TDHSAudioStretch(min_period, max_period, self.num_channels, flags)
        
        try:
            # Process audio
            output_samples = self._process_with_stretcher_float(
                stretcher, samples_float, ratio
            )
            
            # Restore channel-first layout
            self.samples = self._deinterleave(output_samples)
            
        finally:
            stretcher.deinit()

    def _interleave(self, samples: np.ndarray) -> np.ndarray:
        """Flatten channel-first samples to contiguous interleaved float32."""
        if samples.dtype != np.float32:
            samples = samples.astype(np.float32, copy=False)

        if self.num_channels == 1:
            return np.ascontiguousarray(samples[0], dtype=np.float32)
        elif self.num_channels == 2:
            # Interleave L,R,L,R...
            return np.ascontiguousarray(samples.T.ravel(), dtype=np.float32)
        else:
            raise ValueError(f"Unsupported channel count: {self.num_channels}")

    def _deinterleave(self, samples: np.ndarray) -> np.ndarray:
        """Restore contiguous channel-first float32 samples from interleaved input."""
        samples = np.asarray(samples, dtype=np.float32)
        if self.num_channels == 1:
            return np.ascontiguousarray(samples.reshape(1, -1))
        elif self.num_channels == 2:
            # De-interleave L,R,L,R... to (2, N)
            return np.ascontiguousarray(samples.reshape(-1, 2).T)
        else:
            raise ValueError(f"Unsupported channel count: {self.num_channels}")

    def _process_with_stretcher_float(
        self, 
        stretcher: TDHSAudioStretch,
        samples_float: np.ndarray,
        ratio: float
    ) -> np.ndarray:
        """Process float32 samples using the TDHS stretcher."""
        num_input_frames = len(samples_float) // self.num_channels
        
        # Calculate output buffer capacity
        max_ratio_for_capacity = 4.0 if ratio > 2.0 or ratio < 0.5 else 2.0
        effective_max_ratio = max(ratio, max_ratio_for_capacity if ratio > 1.0 else 1.0 / ratio)
        
        output_capacity = stretcher.output_capacity(num_input_frames, effective_max_ratio)
        output_buffer = np.zeros(output_capacity * self.num_channels, dtype=np.float32)
        
        # Process samples
        num_processed = stretcher.process_samples_float(
            samples_float, num_input_frames, output_buffer, ratio
        )
        
        # Flush remaining samples
        flush_buffer = np.zeros(output_capacity * self.num_channels, dtype=np.float32)
        num_flushed = stretcher.flush_float(flush_buffer)
        
        # Combine processed and flushed samples
        processed_size = num_processed * self.num_channels
        flushed_size = num_flushed * self.num_channels
        result = np.empty(processed_size + flushed_size, dtype=np.float32)
        
        result[:processed_size] = output_buffer[:processed_size]
        result[processed_size:processed_size + flushed_size] = flush_buffer[:flushed_size]
        
        return result


def stretch_audio(
    input_path: Union[str, Path],
    output_path: Union[str, Path],
    ratio: float = 1.0,
    gap_ratio: float = 0.0,
    upper_freq: int = 333,
    lower_freq: int = 55,
    buffer_ms: float = 25.0,
    threshold_gap_db: float = -40.0,
    double_range: bool = False,
    fast_detection: bool = False,
    normal_detection: bool = False,
    sample_rate: int = 0,
) -> None:
    """
    AudioStretchy convenience function to stretch an audio file.

    Args:
        input_path: Path to input audio file
        output_path: Path for output audio file
        ratio: Stretch ratio (>1.0 = slower, <1.0 = faster)
        gap_ratio: Separate ratio for silent sections (0.0 = use main ratio)
        upper_freq: Upper frequency limit for period detection (Hz)
        lower_freq: Lower frequency limit for period detection (Hz)
        buffer_ms: Buffer size for silence detection (currently unused)
        threshold_gap_db: Silence threshold in dB (currently unused)
        double_range: Enable extended ratio range (0.25-4.0)
        fast_detection: Use faster period detection algorithm
        normal_detection: Force normal detection (currently unused)
        sample_rate: Target sample rate for output (0 = keep original)
    """
    processor = AudioStretch()
    
    # Load audio
    processor.open(input_path)
    
    # Stretch audio
    processor.stretch(
        ratio=ratio,
        gap_ratio=gap_ratio,
        upper_freq=upper_freq,
        lower_freq=lower_freq,
        buffer_ms=buffer_ms,
        threshold_gap_db=threshold_gap_db,
        double_range=double_range,
        fast_detection=fast_detection,
        normal_detection=normal_detection,
    )
    
    # Resample if requested
    if sample_rate > 0 and sample_rate != processor.samplerate:
        processor.resample(sample_rate)
    
    # Save result
    processor.save(output_path)
