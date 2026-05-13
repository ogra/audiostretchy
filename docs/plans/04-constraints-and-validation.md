# 04. 制限事項と検証

## 4.1 メモリ使用量の変化

### 単純比較

| バッファ | 旧 (`int16`) | 新 (`float32`) | 増加率 |
| --- | --- | --- | --- |
| 入力インターリーブ (`samples_*`) | 2 bytes/sample | 4 bytes/sample | **×2** |
| 出力 `output_buffer` | 2 bytes/sample × capacity | 4 bytes/sample × capacity | **×2** |
| フラッシュ `flush_buffer` | 2 bytes/sample × capacity | 4 bytes/sample × capacity | **×2** |
| 連結 `result` | 2 bytes/sample | 4 bytes/sample | **×2** |

ただし、これまでも `AudioStretch.samples` (`np.float32`) は常駐していたため、
**ピーク使用量は丸ごと 2 倍にはならない**。

### 実効的なピーク

例として 1 分 / ステレオ / 44.1 kHz のソースを `ratio=2.0` で伸縮する場合:

- フレーム数 N ≈ 2.65 M
- `self.samples` (float32): 2 × N × 4 = **約 21.2 MB** (旧設計と同じ)
- `samples_float` (interleaved): 2 × N × 4 = **約 21.2 MB** (旧 int16 では 10.6 MB)
- `output_buffer` + `flush_buffer` (`capacity ≈ 2N`): 2 × (2 × 2N) × 4 = **約 84.8 MB**
  (旧 int16 では 42.4 MB)
- `result` (concat): 約 42.4 MB (旧 21.2 MB)

ピークでは **旧設計比 +約 75 MB 程度** (≒ +85%) の増加が想定される。
モバイル / 組み込み環境を除き、現代のデスクトップ環境では問題にならない範囲。

### 削減手段 (必要な場合)

- `output_capacity` の `max_ratio` 引数を実際の `ratio` ベースで厳しめに見積もる
  (現状コードは安全側に `4.0` / `2.0` を取り得る)。
- 出力をチャンク処理し、各 chunk 終了後に `output_buffer` を再利用する
  (将来的な `gap_ratio` セグメンテーション実装と同時に検討)。

## 4.2 演算パフォーマンス

### 期待される変化

- **C ライブラリ内のコア処理**: 上流 `ogra/feat-implement-float32` の実装次第。一般に
  TDHS は乗算と差分が主で、現代の x86_64 / ARM では SIMD (SSE/AVX/NEON) を活かすと
  float の方が早いケースもある。ただし、CPython 上の呼び出しコスト・メモリ帯域
  ボトルネックの方が支配的なので、**全体としては誤差レベル** と見込む。
- **Python 側の型変換 (撤去)**: `np.clip` + `* 32767` + `astype(np.int16)` の往復が
  なくなるため、**Python 側のオーバーヘッドは確実に減る**。長尺ファイルでは
  数百 ms オーダーの短縮が見込める。
- **メモリ帯域**: バッファサイズが 2 倍になるため、L2/L3 キャッシュに収まりにくい
  ケース (10 秒級以上のフル展開バッファ) ではキャッシュミス増の悪影響が出る可能性。
  C 側がチャンク処理しているなら影響は限定的。

### ベンチ案

`tests/test_performance.py` に float32 ラウンドトリップのベンチを追加し、
旧実装比で:

- ラップタイム: ±10% 以内であること。
- メモリピーク: +100% を超えないこと。

を回帰チェックとして導入することを推奨する。

## 4.3 数値精度上の注意

### 改善点

- 旧設計の `(x * 32767).astype(np.int16)` → `int16 / 32767.0` は、最良でも
  約 **-90 dBFS** の量子化ノイズを乗せる。float32 ネイティブ化により **このノイズは
  完全に消える**。
- 24-bit / 32-bit float 入力のダイナミックレンジ (24-bit で約 -144 dBFS、float32 で
  数百 dB) が、これまで int16 経路で **-90 dBFS にクリップされていた** のが解消する。

### 注意点 (退行ではない)

- float32 のサンプル値は `[-1.0, +1.0]` を **超える** ことがある (ヘッドルーム /
  intersample peak 等)。旧設計は `np.clip(samples, -1.0, 1.0)` で強制クランプ
  していたため、過大入力でもクリッピング歪み程度に留まっていた。
- 新設計では C ライブラリにそのまま渡るため、TDHS の内部で適切に扱われるかを
  確認する必要がある (`ogra/audio-stretch:ogra/feat-implement-float32` の挙動依存)。
- 必要に応じて、`AudioStretch.save()` 直前で `np.clip` を入れる (特に `bit_depth=16`
  で書き出す場合) 互換オプションを追加する。

### NaN / Inf の伝播

- int16 経路では `NaN`/`Inf` は `astype(np.int16)` で実装依存値 (通常 0) に
  なっていた。float32 経路ではそのまま伝播するため、上流側で `NaN`/`Inf` を生む
  バグがあるとそれが顕在化しうる。`AudioStretch.open()` 直後に `np.nan_to_num` で
  サニタイズするオプションを検討してもよい (デフォルト off)。

## 4.4 後方互換性チェックリスト

- [ ] `c_interface/wrapper.py` の `process_samples` / `flush` (int16) を残す。
- [ ] `interface/tdhs.py` の `process_samples` / `flush` (int16) を残す。
- [ ] `AudioStretch.open()` / `AudioStretch.save()` の **既存シグネチャを維持** し、
  新規引数 (`bit_depth`) はデフォルト値ありで追加する。
- [ ] CLI (`__main__.py`) の引数は変更しない。
- [ ] 既存テストがすべてグリーン (`pytest -q`)。

## 4.5 検証手順

実装後、以下の手順で検証する。

1. **サブモジュール切り替え + C 再ビルド**
   ```bash
   git submodule sync && git submodule update --init --remote audio-stretch
   python -m audiostretchy.c_interface.build
   ```

2. **ユニットテスト**
   ```bash
   pytest -q tests/
   ```

3. **数値精度回帰テスト** (新規)
   - 既知の 32-bit float WAV (sine sweep 等) を `ratio=1.0` で stretch して
     入出力差が float32 ULP オーダー (`< 1e-6`) であることを確認。

4. **ラウンドトリップビット精度**
   - 16-bit PCM → load → save (bit_depth=16) で、旧実装と新実装の出力 WAV を
     `cmp` し、差分が許容量子化ノイズの範囲に収まっていることを確認。

5. **性能回帰**
   - `tests/test_performance.py` の Wall time / RSS を旧実装 (master) と比較。

6. **CLI スモークテスト**
   ```bash
   python -m audiostretchy tests/audio.wav /tmp/out.wav --ratio 1.2
   ```

7. **CI パイプラインでの検証**
   - 既存の GitHub Actions ワークフロー (`.github/workflows/`) は `pytest` を回しているため、
     上記 (2)〜(3) の追加テストを `tests/` に置けば **自動で float32 経路もカバー** される。
   - サブモジュール更新が CI で確実に走るよう、`actions/checkout` ステップに
     `submodules: recursive` が指定されているか確認すること。指定が無い場合は
     ワークフローを更新する。
   - 各プラットフォーム (Linux / macOS / Windows) で `_stretch_*` 共有ライブラリの
     再ビルドが必要なため、ビルドジョブを Phase 1 完了時点で **一度全 OS 通す** こと。

## 4.6 ロールアウト計画

| フェーズ | 内容 |
| --- | --- |
| Phase 1 | `audio-stretch` サブモジュール切替 + C 再ビルド + CI Green |
| Phase 2 | `c_interface/wrapper.py` に float32 API 追加 (旧 API は維持) |
| Phase 3 | `core.py` を float32 パイプラインに切替 + 既存テスト Green |
| Phase 4 | `save()` の `bit_depth` 引数追加 + 新規 float32 テスト追加 |
| Phase 5 | レガシー `stretch.py` / `interface/tdhs.py` の追従 (または Deprecation) |
| Phase 6 | ドキュメント (`README.md` / `CHANGELOG.md`) 更新 |

各フェーズで小さく PR を分けることで、レビュー負荷とロールバックコストを下げる。
