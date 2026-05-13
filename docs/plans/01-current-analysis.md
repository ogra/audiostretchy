# 01. 現状分析 — `int16` 依存箇所のリストアップ

本ドキュメントでは、`audiostretchy` のソースツリーをスキャンし、
オーディオデータの **読み込み / バッファ確保 / 信号処理 / 書き出し** において
`int16` (`np.int16` / `short` / `ctypes.c_int16`) に依存している箇所を列挙する。

## 1.1 リポジトリ構造 (関連部分)

```
src/audiostretchy/
├── __init__.py            # public API のエクスポート (core から)
├── __main__.py            # CLI (fire 経由で stretch_audio を呼ぶ)
├── core.py                # ★ 現行のメイン実装。Pedalboard + TDHS の橋渡し
├── stretch.py             # ★ レガシー実装。core.py とほぼ同等の責務
├── c_interface/
│   ├── __init__.py        # TDHSAudioStretch を再エクスポート
│   ├── wrapper.py         # ★ ctypes バインディング (int16 固定)
│   ├── build.py           # C ライブラリのビルド補助
│   └── lib/_stretch_*.{so,dll,dylib}  # コンパイル済み共有ライブラリ
└── interface/
    └── tdhs.py            # ★ レガシーな ctypes バインディング (int16 固定)

audio-stretch/             # サブモジュール: dbry/audio-stretch (現状)
vendors/resample/          # サブモジュール: dbry/audio-resampler (未使用)
```

## 1.2 オーディオ I/O — `float32` で完結している箇所

I/O 自体は **すでに `float32`** で取り扱われている。`int16` 依存ではない。

- `src/audiostretchy/core.py:54-58` — `AudioFile(input_source).read(f.frames)` は
  Pedalboard の `AudioFile` を使っており、戻り値は `(num_channels, num_frames)` の
  `np.float32` 配列。
- `src/audiostretchy/core.py:94-102` — `AudioFile(..., mode="w", ...).write(self.samples)`
  も `float32` 配列を直接書き出せる。
- `src/audiostretchy/stretch.py:60-64` — レガシー側も `PedalboardAudioFile` 経由で
  `float32` を読み込む。

したがって本改修のスコープは **「ファイル I/O」ではなく「コア処理に渡すための
データ変換と C 側 API」** に絞られる。

## 1.3 `int16` に依存している箇所

### (A) C インタフェース層 — `np.int16` がハードコード

#### `src/audiostretchy/c_interface/wrapper.py`

| 行 | 内容 | 備考 |
| --- | --- | --- |
| 104-112 | `stretch_samples.argtypes` に `ndpointer(dtype=np.int16)` を 2 回指定 | 入出力バッファの dtype を固定 |
| 115-119 | `stretch_flush.argtypes` も `ndpointer(dtype=np.int16)` | フラッシュバッファの dtype を固定 |
| 145-164 | `process_samples()` のドキュメント・引数説明が "Input audio samples (int16)" | コメントも int16 前提 |
| 166-176 | `flush()` も同様 | 同上 |

> `stretch_init` / `stretch_output_capacity` / `stretch_reset` / `stretch_deinit` は
> サンプル型に依存しないため、float32 対応でもシグネチャ変更不要。

#### `src/audiostretchy/interface/tdhs.py` (レガシー)

| 行 | 内容 |
| --- | --- |
| 78-84 | `stretch_samples.argtypes` の入出力に `ndpointer(dtype=np.int16)` |
| 87-90 | `stretch_flush.argtypes` の出力に `ndpointer(dtype=np.int16)` |

### (B) コア処理層 — 明示的な int16 ⇄ float32 変換

#### `src/audiostretchy/core.py`

| 行 | 内容 | スケーリング係数 |
| --- | --- | --- |
| 174 | `samples_int16 = self._convert_to_int16(self.samples)` | — |
| 191 | `output_samples = self._process_with_stretcher(stretcher, samples_int16, ratio)` | — |
| 194 | `self.samples = self._convert_from_int16(output_samples)` | — |
| 199-212 | `_convert_to_int16`: `np.clip(samples, -1.0, 1.0)` → `* 32767` → `.astype(np.int16)` | **32767** |
| 214-225 | `_convert_from_int16`: `samples_int16.astype(np.float32) / 32767.0` | **32767.0** |
| 241 | `output_buffer = np.zeros(output_capacity * self.num_channels, dtype=np.int16)` | dtype 固定 |
| 249 | `flush_buffer = np.zeros(output_capacity * self.num_channels, dtype=np.int16)` | dtype 固定 |
| 254 | `result = np.zeros(total_samples * self.num_channels, dtype=np.int16)` | dtype 固定 |

#### `src/audiostretchy/stretch.py` (レガシー)

| 行 | 内容 | スケーリング係数 |
| --- | --- | --- |
| 163 | `normalized_sum = rms_sum / samples / (32768.0 * 32767.0 * 0.5)` | **32768/32767** (RMS 正規化定数; ただしこのメソッドは現状壊れている — `samples` 未定義) |
| 212 | `int16_samples = (self.samples * 32767).astype(np.int16)` | **32767** |
| 217 | `pcm_data_in = np.ascontiguousarray(int16_samples[0, :])` | dtype int16 |
| 220 | `pcm_data_in = np.ascontiguousarray(int16_samples.T.ravel())` | dtype int16 |
| 287 | `pcm_data_out = np.zeros(out_capacity * self.nchannels, dtype=np.int16)` | dtype int16 |
| 296-298 | `pcm_data_flush_out = np.zeros(... dtype=np.int16)` | dtype int16 |
| 313-315 | `float32_output_samples = actual_output_samples_int16.astype(np.float32) / 32767.0` | **32767.0** |

### (C) その他

- `src/audiostretchy/dummy.c` — ビルド用のダミー C ファイル。サンプル型非依存。
- `src/audiostretchy/__main__.py` — CLI ラッパ。型非依存。
- `tests/test_core.py` 他 — `int16` を直接扱う処理は無し (ラウンドトリップ後の数値比較のみ)。

## 1.4 スケーリング係数のまとめ

| 係数 | 出現箇所 | 役割 |
| --- | --- | --- |
| `32767` | `core.py:203`, `stretch.py:212` | float → int16 への up-scale (正のフルスケール) |
| `32767.0` | `core.py:216`, `stretch.py:315` | int16 → float への down-scale |
| `32768.0 * 32767.0 * 0.5` | `stretch.py:163` | RMS 正規化用の二乗フルスケール (壊れた関数内) |

> 注: `int16` の正側ピークは 32767、負側ピークは -32768 と非対称なため、
> 上記の係数を使う限り **完全に対称な round-trip は不可能** であり、約 ±1 LSB の
> 誤差 (≒ -90 dBFS) が常に乗る。これが本改修で除去される最大の品質改善ポイント。

## 1.5 `dbry/audio-resampler` (`vendors/resample`) の参照状況

- `src/` 配下を `grep` した結果、Python コードから `audio-resampler` を **直接参照する記述は無い**。
- リサンプリングは `core.py:124-125` および `stretch.py` で `pedalboard.Resample` を使用しており、
  C 側 `audio-resampler` に依存していない。
- したがって本改修において `vendors/resample` の API 変更や float32 対応は **不要**。
  ただし、将来的に `pedalboard` 依存をやめて C 実装に切り替える場合は別途検討が必要。

## 1.6 上流 `ogra/audio-stretch:ogra/feat-implement-float32` の API

`stretch.h` の抜粋:

```c
// Original int16 API (maintained for backward compatibility)
int stretch_samples (StretchHandle handle, const int16_t *samples, int num_samples, int16_t *output, float ratio);
int stretch_flush   (StretchHandle handle, int16_t *output);

// New float32 API (preferred for new code)
int stretch_samples_float (StretchHandle handle, const float *samples, int num_samples, float *output, float ratio);
int stretch_flush_float   (StretchHandle handle, float *output);
```

- `stretch_init` / `stretch_output_capacity` / `stretch_reset` / `stretch_deinit` は
  従来通り (サンプル型非依存)。
- 旧 `int16` API も維持されているため、**段階的に Python 側を切り替えてもバイナリ
  互換性が壊れることはない**。
