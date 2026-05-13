# 03. 具体的な実装指示

`02-design.md` で示した方針を、ファイル単位の `diff` / コードブロックに落とし込む。
本ドキュメントの diff は **適用順** に並べてある。

> 注: diff は説明用であり、実装時は `report_progress` / `edit` ツールで適用する。
> 行番号は本リポジトリのコミット時点のもの。

## 3.1 `audio-stretch` サブモジュールの差し替え

### `.gitmodules`

```diff
 [submodule "audio-stretch"]
     path = audio-stretch
-    url = https://github.com/dbry/audio-stretch.git
+    url = https://github.com/ogra/audio-stretch.git
+    branch = ogra/feat-implement-float32
```

その後、ローカル/CI でサブモジュールを更新:

```bash
git submodule sync
git submodule update --init --remote audio-stretch
# 共有ライブラリの再ビルド
python -m audiostretchy.c_interface.build   # もしくは scripts/compile_c.py
```

## 3.2 ctypes ラッパーへの float32 API 追加

### `src/audiostretchy/c_interface/wrapper.py`

#### (a) `_setup_function_signatures` に float 関数を追加

```diff
@@ src/audiostretchy/c_interface/wrapper.py
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

+        # stretch_samples_float (float32 API, preferred)
+        self.stretch_samples_float = self._lib.stretch_samples_float
+        self.stretch_samples_float.argtypes = [
+            ctypes.c_void_p,
+            np.ctypeslib.ndpointer(dtype=np.float32),
+            ctypes.c_int,
+            np.ctypeslib.ndpointer(dtype=np.float32),
+            ctypes.c_float,
+        ]
+        self.stretch_samples_float.restype = ctypes.c_int
+
         # stretch_flush
         self.stretch_flush = self._lib.stretch_flush
         self.stretch_flush.argtypes = [
             ctypes.c_void_p,
             np.ctypeslib.ndpointer(dtype=np.int16),
         ]
         self.stretch_flush.restype = ctypes.c_int

+        # stretch_flush_float (float32 API, preferred)
+        self.stretch_flush_float = self._lib.stretch_flush_float
+        self.stretch_flush_float.argtypes = [
+            ctypes.c_void_p,
+            np.ctypeslib.ndpointer(dtype=np.float32),
+        ]
+        self.stretch_flush_float.restype = ctypes.c_int
+
```

#### (b) `process_samples_float` / `flush_float` の Python メソッドを追加

```diff
     def process_samples(
         self,
         samples: np.ndarray,
         num_samples: int,
         output: np.ndarray,
         ratio: float,
     ) -> int:
         ...
         return self.stretch_samples(self.handle, samples, num_samples, output, ratio)

+    def process_samples_float(
+        self,
+        samples: np.ndarray,
+        num_samples: int,
+        output: np.ndarray,
+        ratio: float,
+    ) -> int:
+        """
+        Process audio samples (float32) with specified stretch ratio.
+
+        Args:
+            samples: Input audio samples (float32, interleaved if stereo)
+            num_samples: Number of samples per channel
+            output: Output buffer (float32)
+            ratio: Stretch ratio (>1.0 = slower, <1.0 = faster)
+
+        Returns:
+            Number of output samples produced
+        """
+        return self.stretch_samples_float(
+            self.handle, samples, num_samples, output, ratio
+        )
+
     def flush(self, output: np.ndarray) -> int:
         ...
         return self.stretch_flush(self.handle, output)

+    def flush_float(self, output: np.ndarray) -> int:
+        """
+        Flush remaining samples from internal buffers (float32 API).
+
+        Args:
+            output: Output buffer (float32)
+
+        Returns:
+            Number of flushed samples
+        """
+        return self.stretch_flush_float(self.handle, output)
+
```

> **後方互換**: 既存の `process_samples` / `flush` (int16) はそのまま残す。

## 3.3 コア処理を float32 ベースに切り替え

### `src/audiostretchy/core.py`

#### (a) `stretch` メソッドから int16 変換を撤去

```diff
@@ src/audiostretchy/core.py
         if self.samples is None:
             raise ValueError("No audio data to stretch. Call open() first")

         if ratio <= 0:
             raise ValueError("Stretch ratio must be positive")

         # Skip processing if no change needed
         effective_gap_ratio = gap_ratio if gap_ratio > 0 else ratio
         if ratio == 1.0 and effective_gap_ratio == 1.0:
             return

-        # Convert float32 samples to int16 for C library
-        samples_int16 = self._convert_to_int16(self.samples)
+        # Interleave float32 samples for the C library (no dtype conversion).
+        samples_float = self._interleave(self.samples)

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
-            # Process audio
-            output_samples = self._process_with_stretcher(stretcher, samples_int16, ratio)
-
-            # Convert back to float32 and update samples
-            self.samples = self._convert_from_int16(output_samples)
+            # Process audio in native float32
+            output_samples = self._process_with_stretcher_float(
+                stretcher, samples_float, ratio
+            )
+            # Restore (channels, frames) layout
+            self.samples = self._deinterleave(output_samples)

         finally:
             stretcher.deinit()
```

#### (b) 旧 `_convert_to_int16` / `_convert_from_int16` を `_interleave` / `_deinterleave` に置換

```diff
-    def _convert_to_int16(self, samples: np.ndarray) -> np.ndarray:
-        """Convert float32 samples to int16 format expected by C library."""
-        # Clip to valid range and convert
-        samples_clipped = np.clip(samples, -1.0, 1.0)
-        samples_int16 = (samples_clipped * 32767).astype(np.int16)
-
-        # Interleave channels if stereo
-        if self.num_channels == 1:
-            return np.ascontiguousarray(samples_int16[0])
-        elif self.num_channels == 2:
-            # Interleave L,R,L,R...
-            return np.ascontiguousarray(samples_int16.T.ravel())
-        else:
-            raise ValueError(f"Unsupported channel count: {self.num_channels}")
-
-    def _convert_from_int16(self, samples_int16: np.ndarray) -> np.ndarray:
-        """Convert int16 samples back to float32 format."""
-        samples_float32 = samples_int16.astype(np.float32) / 32767.0
-
-        # De-interleave channels if stereo
-        if self.num_channels == 1:
-            return samples_float32.reshape(1, -1)
-        elif self.num_channels == 2:
-            # De-interleave L,R,L,R... to (2, N)
-            return samples_float32.reshape(-1, 2).T
-        else:
-            raise ValueError(f"Unsupported channel count: {self.num_channels}")
+    def _interleave(self, samples: np.ndarray) -> np.ndarray:
+        """Flatten (channels, frames) float32 array to interleaved 1D float32."""
+        if samples.dtype != np.float32:
+            samples = samples.astype(np.float32, copy=False)
+        if self.num_channels == 1:
+            return np.ascontiguousarray(samples[0])
+        elif self.num_channels == 2:
+            return np.ascontiguousarray(samples.T.ravel())
+        else:
+            raise ValueError(f"Unsupported channel count: {self.num_channels}")
+
+    def _deinterleave(self, samples: np.ndarray) -> np.ndarray:
+        """Inverse of _interleave: returns (channels, frames) float32 array."""
+        if self.num_channels == 1:
+            return samples.reshape(1, -1)
+        elif self.num_channels == 2:
+            return samples.reshape(-1, 2).T
+        else:
+            raise ValueError(f"Unsupported channel count: {self.num_channels}")
```

#### (c) `_process_with_stretcher` を float32 版へ置換

```diff
-    def _process_with_stretcher(
-        self,
-        stretcher: TDHSAudioStretch,
-        samples_int16: np.ndarray,
-        ratio: float,
-    ) -> np.ndarray:
-        """Process samples using the TDHS stretcher."""
-        num_input_frames = len(samples_int16) // self.num_channels
-
-        # Calculate output buffer capacity
-        max_ratio_for_capacity = 4.0 if ratio > 2.0 or ratio < 0.5 else 2.0
-        effective_max_ratio = max(ratio, max_ratio_for_capacity if ratio > 1.0 else 1.0 / ratio)
-
-        output_capacity = stretcher.output_capacity(num_input_frames, effective_max_ratio)
-        output_buffer = np.zeros(output_capacity * self.num_channels, dtype=np.int16)
-
-        # Process samples
-        num_processed = stretcher.process_samples(
-            samples_int16, num_input_frames, output_buffer, ratio
-        )
-
-        # Flush remaining samples
-        flush_buffer = np.zeros(output_capacity * self.num_channels, dtype=np.int16)
-        num_flushed = stretcher.flush(flush_buffer)
-
-        # Combine processed and flushed samples
-        total_samples = num_processed + num_flushed
-        result = np.zeros(total_samples * self.num_channels, dtype=np.int16)
-
-        processed_size = num_processed * self.num_channels
-        flushed_size = num_flushed * self.num_channels
-
-        result[:processed_size] = output_buffer[:processed_size]
-        result[processed_size:processed_size + flushed_size] = flush_buffer[:flushed_size]
-
-        return result
+    def _process_with_stretcher_float(
+        self,
+        stretcher: TDHSAudioStretch,
+        samples_float: np.ndarray,
+        ratio: float,
+    ) -> np.ndarray:
+        """Process float32 samples using the TDHS stretcher (float32 API)."""
+        num_input_frames = len(samples_float) // self.num_channels
+
+        # Calculate output buffer capacity (sample count, dtype-independent)
+        max_ratio_for_capacity = 4.0 if ratio > 2.0 or ratio < 0.5 else 2.0
+        effective_max_ratio = max(
+            ratio, max_ratio_for_capacity if ratio > 1.0 else 1.0 / ratio
+        )
+
+        output_capacity = stretcher.output_capacity(
+            num_input_frames, effective_max_ratio
+        )
+        output_buffer = np.zeros(
+            output_capacity * self.num_channels, dtype=np.float32
+        )
+
+        # Process samples (float32)
+        num_processed = stretcher.process_samples_float(
+            samples_float, num_input_frames, output_buffer, ratio
+        )
+
+        # Flush remaining samples (float32)
+        flush_buffer = np.zeros(
+            output_capacity * self.num_channels, dtype=np.float32
+        )
+        num_flushed = stretcher.flush_float(flush_buffer)
+
+        # Concatenate processed and flushed samples
+        processed_size = num_processed * self.num_channels
+        flushed_size = num_flushed * self.num_channels
+        result = np.empty(processed_size + flushed_size, dtype=np.float32)
+        result[:processed_size] = output_buffer[:processed_size]
+        result[processed_size:processed_size + flushed_size] = flush_buffer[:flushed_size]
+        return result
```

#### (d) `save()` に `bit_depth` 引数を追加 (任意 / 推奨)

```diff
     def save(
         self,
         path: Optional[Union[str, Path]] = None,
         file: Optional[BinaryIO] = None,
         format: Optional[str] = None,
+        bit_depth: Optional[int] = None,
     ) -> None:
         ...
         output_target = file if file is not None else str(path)

         # Infer format from path extension if not specified
         if format is None and path is not None:
             format = Path(path).suffix.lstrip(".")

+        # For WAV, default to 32-bit float to preserve internal precision.
+        # For other formats, leave bit_depth=None so Pedalboard picks a default.
+        write_kwargs: dict = {}
+        effective_bit_depth = bit_depth
+        if effective_bit_depth is None and (format or "").lower() == "wav":
+            effective_bit_depth = 32
+        if effective_bit_depth is not None:
+            write_kwargs["bit_depth"] = effective_bit_depth
+
         try:
             with AudioFile(
                 output_target,
                 mode="w",
                 samplerate=self.samplerate,
                 num_channels=self.num_channels,
                 format=format,
+                **write_kwargs,
             ) as f:
                 f.write(self.samples)
```

> 既存テストとの互換性に配慮し、デフォルトでは WAV 出力のみ 32-bit float に昇格させる。
> ユーザーが `bit_depth=16` を指定すれば従来動作に戻せる。

## 3.4 レガシー `stretch.py` / `interface/tdhs.py` の追従

### `src/audiostretchy/interface/tdhs.py`

`c_interface/wrapper.py` と同様に、`stretch_samples_float` / `stretch_flush_float` の
`argtypes` (`np.float32`) を追加し、`process_samples_float` / `flush_float` メソッドを
追加する。

### `src/audiostretchy/stretch.py`

`stretch` メソッド内の `int16` 変換 (212 行付近 / 287-298 行 / 313-315 行) を、
`core.py` と同様に **インターリーブ整形のみ** + `process_samples_float` /
`flush_float` 呼び出しに置換する。

> なお 139-168 行の `resample` メソッドは現状壊れている (`samples` 未定義) ため、
> 本改修のスコープでは触らず、別 issue でフォローする。

## 3.5 テストの確認

`tests/` 配下を確認し、`int16` の dtype を直接アサートしているテストがあれば
更新する。`tests/test_core.py` を grep した限り、`np.int16` を直接検査する記述は
ないため、追加変更は不要の見込み。新規テストとして:

- `tests/test_core_float32.py` を追加し、
  - 32-bit float WAV を読み書きしてラウンドトリップ誤差が **ビット完全** に近いこと
    (`np.allclose(in_, out, atol=1e-6)`) を確認。
  - `bit_depth=32` 指定で保存した WAV を `pedalboard.AudioFile` で読み戻し、
    `fileformat`/`bit_depth` を確認。

を推奨する。
