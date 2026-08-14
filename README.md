# EPSGemuEngine

**FmEngineApi** 準拠の AY8930 (EPSG) エミュレーションエンジン。
MAME 由来の ay8910 コア ([furnace](https://github.com/tildearrow/furnace) fork) を
統合した共有ライブラリ (DLL / .so / .dylib) です。

## 統合コア

| パス | 由来 | チップ | FmEngineApi チップ名 |
|---|---|---|---|
| `extern/ay8910` | [furnace](https://github.com/tildearrow/furnace) `platform/sound/ay8910` ← [MAME](https://github.com/mamedev/mame) `sound/ay8910` | AY8930 / YM2149 / AY-3-8910 / AY-3-8914 | `EPSG` / `SSG` / `PSG` / `PSG2` |

## 対応チップ一覧

| チップ名 | 実チップ | デフォルトクロック | ネイティブレート |
|---|---|---|---|
| `EPSG` | AY8930     | 2.000 MHz | clock / 4 (500,000 Hz) |
| `SSG`  | YM2149     | 2.000 MHz | clock / 8 (250,000 Hz) |
| `PSG`  | AY-3-8910  | 2.000 MHz | clock / 8 (250,000 Hz) |
| `PSG2` | AY-3-8914  | 2.000 MHz | clock / 8 (250,000 Hz) |

クロックは `FmEngine_AddChip` の `clock` 引数で変更できます (0 でデフォルト)。

## ファイル構成

```
EPSGemuEngine/
├── CMakeLists.txt
├── README.md
├── extern/
│   └── ay8910/               ← フォークしたコア (無改変)
│       ├── ay8910.h
│       ├── ay8910.cpp
│       └── README.md         ← 由来と上流追従の手順
├── patches/
│   └── epsg.json             ← FMEngineTest 用テストパッチ
└── src/
    ├── FmEngineApi.h         ← API ヘッダ (FMEngineTest と共通)
    └── EPSGemuEngine.cpp     ← エンジン実装
```

## ビルド

### Windows (Visual Studio 2022)

```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
:: 成果物: build\bin\Release\EPSGemuEngine.dll
```

### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
# 成果物: build/bin/libEPSGemuEngine.so  (Linux)
#         build/bin/libEPSGemuEngine.dylib (macOS)
```

## FMEngineTest との接続

ビルドした共有ライブラリを FMEngineTest の実行ディレクトリに置き、
`-e` オプションで指定します。

```cmd
copy build\bin\Release\EPSGemuEngine.dll <FMEngineTest_dir>
cd <FMEngineTest_dir>
FMEngineTest -e EPSGemuEngine.dll <EPSGemuEngine_dir>\patches\epsg.json
```

`SSG` は他エンジンと同じチップ名なので、`patches/ssg.json` をそのまま
本エンジンに対して実行して比較できます。

## レジスタマップ (EPSG)

### 互換モード (AY-3-8910 互換)

| レジスタ | 内容 |
|---|---|
| `0x00` / `0x01` | CH A トーン周期 TP (12bit: fine + coarse 下位 4bit) |
| `0x02` / `0x03` | CH B トーン周期 |
| `0x04` / `0x05` | CH C トーン周期 |
| `0x06` | ノイズ周期 NP (5bit) |
| `0x07` | ミキサ (bit0–2 トーン A–C, bit3–5 ノイズ A–C。**負論理**) |
| `0x08`–`0x0A` | CH A–C 音量 (bit0–3 音量, bit4 エンベロープ選択) |
| `0x0B` / `0x0C` | エンベロープ周期 EP (16bit) |
| `0x0D` | bit7–4 モード / bit3–0 エンベロープ波形 |
| `0x0E` / `0x0F` | I/O ポート A / B |

### 拡張モード

`0x0D` の上位ニブルで拡張モードとレジスタバンクを選択します。

| `0x0D` 上位ニブル | 状態 |
|---|---|
| `0x0` – `0x9`, `0xC` – `0xF` | 互換モード |
| `0xA` | 拡張モード / バンク 0 |
| `0xB` | 拡張モード / バンク 1 |

拡張モードとの間でモードが切り替わると、両バンクのレジスタ `0x00`–`0x0C` が
0 にクリアされます。モードを切り替えてから各レジスタを設定してください。

バンク 0 は互換モードと同じ配置ですが、以下が拡張されます。

| レジスタ | 拡張内容 |
|---|---|
| `0x01` / `0x03` / `0x05` | トーン周期 coarse が 8bit (TP は 16bit) |
| `0x06` | ノイズ周期 NP が 8bit |
| `0x08`–`0x0A` | bit0–4 音量 (5bit), bit5 エンベロープ選択 |

バンク 1 (`0x0D` に `0xB0` を書いてから access):

| レジスタ | 内容 |
|---|---|
| `0x00` / `0x01` | CH B エンベロープ周期 |
| `0x02` / `0x03` | CH C エンベロープ周期 |
| `0x04` | CH B エンベロープ波形 |
| `0x05` | CH C エンベロープ波形 |
| `0x06`–`0x08` | CH A–C デューティ比 |
| `0x09` | ノイズ AND マスク |
| `0x0A` | ノイズ OR マスク |
| `0x0D` | バンク 0 と共有 (モード / エンベロープ A 波形) |
| `0x0F` | テストレジスタ |

デューティ比の値と比率:

| 値 | `0` | `1` | `2` | `3` | `4` | `5` | `6` | `7` | `8`以上 |
|---|---|---|---|---|---|---|---|---|---|
| 比率 | 3.125% | 6.25% | 12.5% | 25% | 50% | 75% | 87.5% | 93.75% | 96.875% |

### 発音周波数

| 対象 | 式 |
|---|---|
| トーン (互換モード) | `f = clock / (16 × TP)` |
| トーン (拡張モード) | `f = clock / (8 × TP)` |
| ノイズ (互換モード) | `f = clock / (16 × NP)` |
| エンベロープ | `f = clock / (256 × EP)` |

拡張モードのノイズは、`clock / (4 × NP)` で進むカウンタが
`(LFSR 下位 8bit AND ANDマスク) OR ORマスク` に達するたびに出力を反転します。
マスクにより特定周期を抜き出して矩形波状のノイズを作れます。

`SSG` / `PSG` / `PSG2` はトーン `f = clock / (16 × TP)`、ノイズ `f = clock / (16 × NP)`、
エンベロープ `f = clock / (256 × EP)` です。

## レジスタマップ (PSG2)

AY-3-8914 は AY-3-8910 とレジスタ配置が異なります。

| レジスタ | 内容 |
|---|---|
| `0x00` / `0x04` | CH A トーン周期 (fine / coarse) |
| `0x01` / `0x05` | CH B トーン周期 |
| `0x02` / `0x06` | CH C トーン周期 |
| `0x03` / `0x07` | エンベロープ周期 |
| `0x08` | ミキサ (bit0–2 トーン A–C, bit3–5 ノイズ A–C。**負論理**) |
| `0x09` | ノイズ周期 |
| `0x0A` | エンベロープ波形 |
| `0x0B`–`0x0D` | CH A–C 音量 (bit0–3 音量, bit4–5 エンベロープ選択) |
| `0x0E` / `0x0F` | I/O ポート A / B |

音量レジスタの bit4–5 は 2bit のエンベロープ選択です。0 でエンベロープ不使用
(bit0–3 の固定音量)、1 / 2 / 3 でエンベロープ値 (0–15) をそれぞれ 2bit / 1bit / 0bit
右シフトして音量とします。音量カーブが対数的なため、シフトすると大きく減衰します。

## 実装上の注意

### レジスタ書き込み

`EPSG` / `SSG` / `PSG` では `FmEngine_Write` の `reg` がアドレスラッチにそのまま
渡されます。実チップと同様、上位ニブルが 0 以外の値を書くとチップが非選択状態に
なり、次に `0x00`–`0x0F` のアドレスを書くまでデータ書き込みが無視されます。
`EPSG` のバンク 1 のレジスタは `0x10` 以降ではなく、`0x0D` でバンクを切り替えてから
`0x00`–`0x0F` で指定します。

`PSG2` は専用のアドレスデコーダを通すため、`reg` は下位 4bit だけが使われます。

`port` は使用しません。

### 出力

3 チャンネルを合成したモノラル信号を L/R に振り分け、`FmEngine_SetGain` の
ゲインを適用します。1 チャンネル最大音量時の振幅は 0.125、3 チャンネル同時では
0.375 です。この音量は同じチップを扱う他エンジン (DSAemuEngine の `SSG`) と揃えてあります。

チップ出力は片極性 (無音側が負電位相当) のため、無音時のレベルを差し引いて
出力の DC を除去しています。発音中に残る直流成分は実チップと同じ挙動です。

ネイティブレートからエンジンのサンプルレートへの変換は区間平均で行います。

### 外部メモリ

外部メモリを持たないため、`FmEngine_SetMemory` は `FM_ERR_UNAVAILABLE` を返します。

## テストパッチ

| ファイル | 内容 |
|---|---|
| `patches/epsg.json` | `EPSG` の互換モード (トーン / ノイズ / エンベロープ) と拡張モード (デューティ比 / 5bit 音量 / チャンネル別エンベロープ / ノイズ AND-OR マスク) |
| `patches/psg2.json` | `PSG2` のトーン / ノイズ / エンベロープ (2bit エンベロープ選択の違いを含む) |

## ライセンス

| 対象 | ライセンス |
|---|---|
| 本体 (`src/`, `patches/`, ビルドスクリプト, ドキュメント) | **MIT** — [LICENSE](LICENSE) |
| `extern/ay8910` (MAME / furnace 由来のコア) | **BSD-3-Clause** — [extern/ay8910/LICENSE](extern/ay8910/LICENSE) |

DLL などのバイナリを配布する場合は、コアの著作権表示・条件文・免責を
ドキュメント等に同梱してください。コアの由来は
[extern/ay8910/README.md](extern/ay8910/README.md) を参照してください。
