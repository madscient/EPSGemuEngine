# ay8910 コア

AY8930 / YM2149 / AY-3-8910 / AY-3-8914 のエミュレーションコア。

## 由来

| 項目 | 内容 |
|---|---|
| 取得元 | [tildearrow/furnace](https://github.com/tildearrow/furnace) `src/engine/platform/sound/ay8910.{h,cpp}` |
| 取得時点 | master `c5a61f14d525b73b8877729d4a532bbd375ab90d` |
| 元実装 | [mamedev/mame](https://github.com/mamedev/mame) `src/devices/sound/ay8910.cpp` (Couriersud) |
| ライセンス | BSD-3-Clause ([LICENSE](LICENSE)) |

MAME 版から MAME デバイスフレームワーク依存を除いたスタンドアロン版で、
AY8930 拡張モードの実装が追加されています。

furnace 本体は GPL-2.0-or-later ですが、この 2 ファイルは MAME 由来のため
ファイル先頭の `// license:BSD-3-Clause` ヘッダのとおり BSD-3-Clause です。

`ay8910.h` / `ay8910.cpp` は取得元から**無改変**です。上流への追従は
同じ 2 ファイルを差し替えるだけで行えます。

## 依存

C++ 標準ライブラリのみ (`<algorithm>` `<stdio.h>` `<math.h>` `<string.h>` `<vector>`)。

## 使い方

`sound_stream_update(short outputs[3], int advance)` は 1 回の呼び出しで
`advance` 内部ステップ分だけ状態を進め、3 チャンネル分の現在値を返します。
内部ステップレートはチップにより異なります。

| デバイス | 内部ステップレート |
|---|---|
| `ay8930_device` | clock / 4 |
| `ym2149_device` | clock / 8 |
| `ay8910_device` | clock / 8 |
| `ay8914_device` | clock / 8 |

`ay8914_device` はレジスタ配置が異なるため、`address_w` / `data_w` ではなく
専用の `write(offset, data)` を使います。
