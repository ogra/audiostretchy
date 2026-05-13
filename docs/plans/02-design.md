# 02. 改修設計 — `float32` 対応の方針

`01-current-analysis.md` で洗い出した `int16` 依存箇所を、どのように `float32`
中心の設計へ置き換えるかを定義する。

## 2.1 データ構造の変更

### 影響範囲

`audiostretchy` 内部で実際に dtype を持つバッファは下記の 4 系統。

1. **入力サンプル** (`AudioStretch.samples`): 既に `np.float32` (Pedalboard 由来)。**変更不要**。
2. **C ライブラリへ渡す入力バッファ**: 現状 `np.int16` → **`np.float32` へ変更**。
3. **C ライブラリの出力バッファ / フラッシュバッファ**: 現状 `np.int16` →
   **`np.float32` へ変更**。
4. **C → Python 戻りの結合バッファ** (`result` in `_process_with_stretcher`):
   現状 `np.int16` → **`np.float32` へ変更**。

### 形状 (shape) は維持

- モノラル: `(num_frames,)` のインターリーブなし 1D 配列。
- ステレオ: `[L, R, L, R, ...]` のインターリーブ 1D 配列。
- 多チャンネル (>2): 現状コードと同様、エラーとして拒否する (上流 C ライブラリの
  制約に従う)。

float32 化しても **インターリーブ規約は変更しない**。これにより
`_convert_to_int16` / `_convert_from_int16` の責務は「dtype 変換 + スケーリング」
から「**インターリーブ整形のみ**」に縮退する。

### `AudioStretch` クラスの状態

- `self.samples`: `np.ndarray[np.float32]`, shape `(num_channels, num_frames)`。**変更なし**。
- `self.samplerate`: `int`。**変更なし**。
- `self.num_channels`: `int`。**変更なし**。

クラスの外部 API シグネチャは破壊しない方針とする (後方互換性)。

## 2.2 WAV ヘッダの処理

WAV のヘッダ書き出し自体は Pedalboard が抽象化しているため、`audiostretchy` の
コードからは `WAVE_FORMAT_PCM` / `WAVE_FORMAT_IEEE_FLOAT` / `WAVE_FORMAT_EXTENSIBLE`
のフォーマットコードを直接扱う必要はない。

### 入力側

- `pedalboard.io.AudioFile(input_source)` は 16-bit PCM / 24-bit PCM / 32-bit float /
  64-bit float のいずれの WAV でも自動判別して `float32` を返す。
- そのため、入力が `WAVE_FORMAT_EXTENSIBLE` (cbSize + SubFormat GUID 付き) であっても、
  `AudioStretch.open()` 側で追加処理は不要。

### 出力側

WAV を 32-bit float で書き出すには、`AudioFile.write` 時に **`bit_depth=32`** を
指定する必要がある (Pedalboard の仕様)。現状 `core.py:95-101` は `bit_depth` を
渡していないため、デフォルトの 16-bit PCM になっている可能性が高い。

#### 設計方針

- `AudioStretch.save()` に **`bit_depth: int | None = None`** 引数を追加する。
- `None` の場合のデフォルトを **入力時に観測した bit depth**、もしくは
  `format == "wav"` のときのみ `32` (float) に切り替える。
- 互換性を優先するなら、デフォルトは現状通り (16-bit) のままで、明示指定で 32-bit
  float を選べるようにする。**推奨はデフォルト 32-bit float**。

> 補足: `WAVE_FORMAT_EXTENSIBLE` (cbSize=22, SubFormat=KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
> は、Pedalboard が必要に応じて自動で出力する。Python 側で手動エンコードする必要はない。

### MP3 / FLAC 等

- MP3: そもそも整数 PCM ベース。`bit_depth` 指定は Pedalboard が無視する。挙動は変わらない。
- FLAC: 16/24-bit 整数のみ。`bit_depth` は 16 か 24 を指定する。
- いずれも今回の改修で **退行は起きない**。

## 2.3 信号処理ロジック

### 固定小数点演算・ビットシフトの有無

Python 側 (`core.py` / `stretch.py`) には **固定小数点演算もビットシフトも存在しない**。
唯一の "固定小数点風" 操作は

```python
samples_int16 = (samples_clipped * 32767).astype(np.int16)
samples_float32 = samples_int16.astype(np.float32) / 32767.0
```

の 2 段スケーリングのみ。これは **両方とも撤去** する。

### 新パイプライン

```
Pedalboard.read (float32)
   └─► np.ascontiguousarray(... ravel)        # インターリーブ整形のみ
         └─► stretch_samples_float (C)        # float32 ネイティブ
               └─► np.ascontiguousarray(... reshape)  # デインターリーブ
                     └─► Pedalboard.write (float32)
```

### `output_capacity` の扱い

`stretch_output_capacity` は **サンプル数** (バイト数ではない) を返すため、
返り値の意味は変わらない。バッファ確保サイズは `n_samples` 単位のまま、
`dtype=np.float32` で確保する。

### `gap_ratio` / `buffer_ms` / `threshold_gap_db`

これらは `01-current-analysis.md` で記載した通り **現状の Python 実装では機能していない**。
今回の改修ではスコープ外とし、コメントは現行どおり「currently unused」を維持する。
将来的に Python 側で RMS ベースのセグメンテーションを実装する場合、float32 で
RMS を計算する方が型変換が無い分シンプルになる (むしろ float32 化のメリット)。

## 2.4 互換性 (ハイブリッド対応)

### 入力 int16 / 内部 float32 / 出力ハイブリッド

可能。設計は次の通り。

| 段階 | 型 | 説明 |
| --- | --- | --- |
| ファイル読み込み | int16 / float32 / その他 | Pedalboard が自動で **float32 に正規化** ([-1.0, +1.0]) |
| 内部処理 (C 呼び出し) | float32 | `stretch_samples_float` を使用 |
| ファイル書き出し | float32 / int16 | `bit_depth` 引数で制御 (デフォルト float32) |

つまり、ユーザーから見た **互換性は完全に維持** され、内部処理は全て float32 で
完結する。

### 後方互換: 旧 `int16` API は残すか?

- `c_interface/wrapper.py` の `process_samples` / `flush` (int16) は **残す** ことを推奨。
  - 外部から `TDHSAudioStretch` を直接利用しているコード (もしあれば) を壊さないため。
- ただし `core.py` 内部からは呼ばれなくなる。
- 新規メソッドとして `process_samples_float` / `flush_float` を追加する。

### レガシー `src/audiostretchy/stretch.py` / `interface/tdhs.py`

これらは `core.py` / `c_interface/wrapper.py` へ統合 (または非推奨化) されつつある
過渡期のコード。`__init__.py` でエクスポートされていないため、最低限の整合性
維持 (壊さない) のみを目標とし、本改修では下記いずれかとする:

- 案 A (推奨): 同様に float32 API に追随させる。
- 案 B: `DeprecationWarning` を出して `core.py` への移行を促す。

実装指示書 (`03-implementation.md`) では **案 A** を採る。

## 2.5 依存関係

### `ogra/audio-stretch` (`ogra/feat-implement-float32`)

- サブモジュール (`audio-stretch/`) の URL/参照を、`dbry/audio-stretch` (master) から
  **`ogra/audio-stretch` の `ogra/feat-implement-float32`** ブランチへ切り替える必要がある。
- `.gitmodules` の変更例:

  ```ini
  [submodule "audio-stretch"]
      path = audio-stretch
      url = https://github.com/ogra/audio-stretch.git
      branch = ogra/feat-implement-float32
  ```

  > **ピン留めの注意**: `branch =` 指定はサブモジュールの追従先を示すだけで、
  > 実際にチェックアウトされるのは `git submodule update` 時点の `audio-stretch`
  > サブモジュールエントリが指す **コミット SHA** である。ブランチは移動・削除
  > されうるため、float32 実装が安定したタイミングで以下のいずれかへ移行することを推奨:
  >
  > - 上流リポジトリで安定タグ (例: `v0.5-float32`) を切り、`branch = <タグ名>` に
  >   置き換える、または
  > - サブモジュールエントリを特定の不変な SHA にピン留めしたまま `branch` 行を削除する。

- 共有ライブラリ (`src/audiostretchy/c_interface/lib/_stretch_*.so` 等) は、
  上記サブモジュールから **再ビルドが必要**。`c_interface/build.py` のロジック自体は
  そのまま使えるはず。

### `dbry/audio-resampler` (`vendors/resample`)

- `01-current-analysis.md` の通り **Python から参照されていない**。
- 改修不要。`.gitmodules` も触らない。
- 仮に将来的に C リサンプラーへ切り替える場合は、別 PR / 別フェーズで対応する。

### Python パッケージ依存

- `numpy`, `pedalboard`, `fire` 等は変更不要。
- `pedalboard.io.AudioFile.write(bit_depth=32)` は古い pedalboard でもサポート済み
  (≥ 0.5)。要 `pyproject.toml` のバージョン確認のみ。

## 2.6 設計サマリ図

```
┌─────────────────────────────────────────────────────────────┐
│                   AudioStretch (Python)                     │
│                                                             │
│   open()  ──►  Pedalboard.AudioFile.read  ──► float32       │
│                                                  │          │
│                                                  ▼          │
│   stretch()  ──►  ravel (interleave)  ──► float32 1D        │
│                                                  │          │
│                                                  ▼          │
│              ctypes → stretch_samples_float ─► float32 out  │
│                                                  │          │
│                                                  ▼          │
│                          reshape (deinterleave) ─► float32  │
│                                                  │          │
│                                                  ▼          │
│   save()  ──►  Pedalboard.AudioFile.write(bit_depth=32)     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```
