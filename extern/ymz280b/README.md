# ymz280b コア

YMZ280B (PCMD8) のエミュレーションコア。

## 由来

| 項目 | 内容 |
|---|---|
| 取得元 | [tildearrow/furnace](https://github.com/tildearrow/furnace) `src/engine/platform/sound/ymz280b.{h,cpp}` |
| 取得時点 | master `8041173eb96f4343352125e7ed785c67577479e3` |
| 元実装 | [mamedev/mame](https://github.com/mamedev/mame) `src/devices/sound/ymz280b.cpp` (Aaron Giles) |
| ライセンス | BSD-3-Clause ([LICENSE](LICENSE)) |

MAME 版から MAME デバイスフレームワーク依存を除いたスタンドアロン版です。

furnace 本体は GPL-2.0-or-later ですが、この 2 ファイルは MAME 由来のため
ファイル先頭の `// license:BSD-3-Clause` ヘッダのとおり BSD-3-Clause です。

`ymz280b.h` / `ymz280b.cpp` は取得元から**無改変**です。上流への追従は
同じ 2 ファイルを差し替えるだけで行えます。

## 依存

C++ 標準ライブラリのみ (`<memory>` `<assert.h>` `<stdio.h>` `<string.h>`)。

## 使い方

```cpp
ymz280b_device dev;
dev.device_start(ext_mem);   // 外部メモリの先頭ポインタ
dev.device_reset();

dev.write(0, reg);           // アドレスライト
dev.write(1, value);         // データライト

short* outputs[16];          // ボイス 0-7 の L/R が交互
dev.sound_stream_update(outputs, samples);
```

内部レートは `clock / 384` です (16.9344MHz なら 44,100Hz)。
発音ピッチは `((FN + 1) × clock / 384) / 256` です。

`sound_stream_update` は 8 ボイス分を**別々のバッファ**に書き出します。
`outputs[v*2]` がボイス v の L、`outputs[v*2+1]` が R です。合成は
呼び出し側で行います。各ボイスの出力はトータルレベルとパンポットを
適用済みで、16bit フルスケールが 1 ボイス分の最大振幅です。

## 注意

外部メモリのアクセスに範囲検査がありません。レジスタに設定された
アドレスがそのまま添字になるため、プログラムが使うアドレス範囲を
覆うだけのバッファを渡す必要があります。
