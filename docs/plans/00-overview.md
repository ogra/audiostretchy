# float32 対応 改修プラン — 概要

このディレクトリには、`ogra/audiostretchy` のオーディオ処理パイプラインを
従来の `int16` (16-bit signed PCM) ベースから `float32` (IEEE 32-bit Float)
ベースへ完全対応させるための解析・設計・実装計画を分割して収録します。

## 背景

- 現状の `audiostretchy` は、ファイル I/O は [`pedalboard`](https://github.com/spotify/pedalboard)
  経由で `float32` を扱えるが、コア部分の C ライブラリ (`audio-stretch` / TDHS) が
  `int16` のみを公開していたため、Python 層で **float32 → int16 → 処理 → float32** という
  ラウンドトリップを行っている。
- 上流の C ライブラリは `ogra/audio-stretch` の `ogra/feat-implement-float32` ブランチで
  すでに `float32` 用 API (`stretch_samples_float`, `stretch_flush_float`) が追加済み。
- そのため、本リポジトリ側 (`ogra/audiostretchy`) も「Python 層で int16 を一切経由しない」
  パイプラインへリファクタする時期にある。

## 関連サブモジュール

`.gitmodules` で参照しているサブモジュールは下記の通り。

| パス | リモート | 用途 |
| --- | --- | --- |
| `audio-stretch` | `https://github.com/dbry/audio-stretch.git` | TDHS 時間伸縮アルゴリズム C 実装 (本改修で `ogra/audio-stretch:ogra/feat-implement-float32` に切替) |
| `vendors/resample` | `https://github.com/dbry/audio-resampler` | リサンプリング C 実装 (現状 Python からは未参照) |

## 文書構成

| ファイル | 内容 |
| --- | --- |
| [`00-overview.md`](./00-overview.md) | 本ドキュメント。全体サマリと目次 |
| [`01-current-analysis.md`](./01-current-analysis.md) | 現状分析: `int16` 依存箇所、構造体・関数シグネチャ・スケーリング係数の一覧 |
| [`02-design.md`](./02-design.md) | 改修設計: データ構造、WAV ヘッダ (`WAVE_FORMAT_IEEE_FLOAT` / `WAVE_FORMAT_EXTENSIBLE`)、信号処理、互換性、依存関係 |
| [`03-implementation.md`](./03-implementation.md) | 具体的な実装指示 (`diff` 形式 / コードブロック) |
| [`04-constraints-and-validation.md`](./04-constraints-and-validation.md) | メモリ使用量・演算性能の注意点、検証方針 |

## 改修方針サマリ (TL;DR)

1. `src/audiostretchy/c_interface/wrapper.py` の ctypes バインディングに
   `stretch_samples_float` / `stretch_flush_float` を追加する。
2. `src/audiostretchy/core.py` の `_convert_to_int16` / `_convert_from_int16` を撤去し、
   `float32` のままで C ライブラリへ渡す `_process_with_stretcher_float` を導入する。
3. 旧 `src/audiostretchy/interface/tdhs.py` および `src/audiostretchy/stretch.py`
   (legacy) も同様に float32 API を使うようリファクタする (もしくは非推奨化)。
4. WAV ヘッダ周りは Pedalboard が抽象化しているため、`AudioFile(... bit_depth=32)`
   など書き込み時のパラメータで `WAVE_FORMAT_IEEE_FLOAT` を明示する。
5. `dbry/audio-resampler` (`vendors/resample`) は **Python から直接参照されていない**ため、
   今回の改修対象外。詳細は `02-design.md` を参照。

## 受け入れ条件 (Definition of Done)

- 通常の WAV / MP3 入力 (内部は `int16` の場合もある) でも、Python 内部処理は
  すべて `float32` で完結すること。
- 32-bit float WAV (`WAVE_FORMAT_IEEE_FLOAT`) を入力・出力できること。
- 既存のユニットテスト (`tests/test_core.py` 等) がすべてグリーン。
- `int16 ⇄ float32` 変換による量子化誤差 (約 -90 dB) が消え、出力品質が向上していること。
