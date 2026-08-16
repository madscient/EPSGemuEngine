# mpeg_audio コア

MPEG audio layer 2 系のデコーダ。Yamaha AMM (AMusement Music compression)
形式のデコードに使います。

## 由来

| 項目 | 内容 |
|---|---|
| 取得元 | [mamedev/mame](https://github.com/mamedev/mame) `src/devices/sound/mpeg_audio.{h,cpp}` (Olivier Galibert) |
| 取得時点 | master `bdf56c6ae80289f7c4f944cd847d8fbeef13b0fc` |
| ライセンス | BSD-3-Clause ([LICENSE](LICENSE)) |

`mpeg_audio.h` / `mpeg_audio.cpp` は取得元から**無改変**です。上流への追従は
同じ 2 ファイルを差し替えるだけで行えます。

## 依存

`mpeg_audio.cpp` が `#include "emu.h"` する MAME のフレームワークヘッダは、
同じディレクトリに置いた [emu.h](emu.h) が肩代わりします。実際に必要なのは
`<cassert>` `<cmath>` `<cstdint>` `<cstring>` だけです。

`std::numbers::pi` を使うため C++20 が必要です。

## 使い方

```cpp
mpeg_audio decoder(rom_base, mpeg_audio::AMM, false, 0);

int pos = bit_offset;      // base からのビット単位オフセット
short out[0x1000];
int samples, rate, channels;
decoder.decode_buffer(pos, rom_size * 8, out, samples, rate, channels, atbl);
```

`decode_buffer` は 1 フレーム (AMM なら最大 1152 サンプル/チャンネル) を
デコードし、`pos` を次のフレーム先頭へ進めます。サンプルレートとチャンネル数は
フレームごとに変わり得ます。1152 未満のサンプル数はストリーム終端を意味します。

`atbl` は AMMSL 系の CBR サンプル用のパラメータ索引です。

新しいフレーズを再生し直すときは `clear()` で内部バッファを初期化します。
