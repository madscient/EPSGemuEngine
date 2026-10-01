# EPSGemuEngine 作業計画・経緯

AI がセッションをまたいで作業を引き継ぐための文書。利用者向けの現在の仕様は
`README.md`、このリポジトリでの作業規則は `CLAUDE.md` にある。

## 確度の読み方

| 印 | 意味 |
|---|---|
| **確認済み** | 走らせて確かめた。何でどう確かめたかを添える |
| **確認済み(読解)** | ソースや文書を読んで確かめた。走らせてはいない |
| **未検証** | 作ったが動かして確かめていない |
| **推測** | 出典を示せない。根拠を一行添える |

## 1. 追随している FmEngineApi の版

| 対象 | 版 | 備考 |
|---|---|---|
| madscient/FMEngineTest `docs/FmEngineApi.md`（仕様の正） | `df7dae8` | 部位ゲイン `e39b206`、外部メモリの割り当て `e002890` までを読んだ |
| madscient/YMEngine `src/FmEngineApi.h`（参照ヘッダ） | `8f81213` | `src/FmEngineApi.h` はこの写しで、一字も変えていない（**確認済み**: `git hash-object` が一致） |

**写していない参照ヘッダの版**: YMEngine `ac29207` は `FmEngine_SetMemoryEx` と
`FmMemoryAccess`・`FM_MEM_ADPCM_B_ROMMODE` を足した。写していない理由:

- 本エンジンは `FmEngine_SetMemoryEx` をエクスポートしない（§2.3）
- `ac29207` のヘッダは `FmEngine_SetMemory` について「data は複製せず参照する」
  「チップからの書き込みは捨てる」と書く。本エンジンの `PCMD8` はチップの書き込み
  （`0x87`）を渡されたバッファに入れるので、写すと注記が挙動と逆になる。仕様書
  （`df7dae8`）はこの点をエンジンのコア実装に任せている（**確認済み(読解)**）

`SetMemoryEx` を実装するときに写し直し、`PCMD8` の `SetMemory` の扱いもそのとき決める。

## 2. 2026-10-02 FmEngineApi の改定への追随

### 2.1 部位ゲイン

- `FmEngine_SetPartGain` / `FmEngine_GetPartGain` / `FmEngine_GetPartMask` を
  エクスポートし、どのチップも部位を持たないとした。`GetPartMask` は `FM_OK` で 0、
  `Set/GetPartGain` は常に `FM_ERR_INVALID_ARG`。未知の `chip_id`・null の出力
  ポインタを `FM_ERR_INVALID_ARG` にするのは YMEngine に揃えた
- 根拠: 仕様書の部位の表に本エンジンのチップは無く、表に無いチップは部位を持たない
  と仕様書にある（**確認済み(読解)**）
- 3 関数は任意のエクスポートで、エクスポートしなくても呼び出し側の扱いは同じ。
  エクスポートした理由: ヘッダ（参照ヘッダの写し）が宣言する関数を DLL も持つように
  するため。仕様が部位ゲインを必須に上げても準拠のままでいられる
- 前提: 仕様書の部位の表に本エンジンのチップが載らないこと。載ったら `Generate` の
  合成を部位ごとに分ける。`Generate` は SSG 部（`chipCalcStereo`）・AMM 部・
  ADPCM 部・PCMD8 を別々の関数で足し込んでいる（**確認済み(読解)**）
- SSGS / SSGS2 の SSG 部と ADPCM 部、SSGS3 の SSG 部と AMM 部が別の端子から出るかは
  データシートで確かめていない（**推測**: SSGS には SSG-ADPCM ミックスレベルの
  レジスタが、SSGS3 には SSG と AMM それぞれのトータルボリューム（`0x32` と `0x01`）が
  あり、チップの中で混ぜていると読める）

### 2.2 外部メモリの種別: `FM_MEM_AMM` を廃止し `FM_MEM_PCM` に統合

- 発端: 仕様書 `e002890` が `FM_MEM_ADPCM_B_ROMMODE = 4` を割り当て、本リポジトリ
  独自の `FM_MEM_AMM = 4` と番号が重なった
- 決定（ユーザー）: AMMS-A / SSGS3 の AMM フレーズ ROM も `FM_MEM_PCM` で受ける。
  分けておく意味が薄いため。SSGS / SSGS2 の ADPCM ボイス ROM と PCMD8 の外部メモリは
  前から `FM_MEM_PCM` だった
- 前提: 1 チップが持つ外部メモリは 1 種類（AMM 部・ADPCM 部・PCMD8 を 2 つ以上持つ
  チップが無い）。`ChipEntry` の `mem` / `mem_size` が 1 組なのもこの前提による。
  崩れたら（2 種類のメモリを持つチップを足すとき）種別を分け、番号は仕様書で
  割り当ててもらう
- 影響: `FM_MEM_AMM`（4）を渡す呼び出し側は `FM_ERR_UNAVAILABLE` になる。手元に
  ある互換エンジン・利用側のリポジトリをソース検索した範囲では、`FM_MEM_AMM` を
  使っているのは本リポジトリだけだった（**確認済み**、2026-10-02）
- 見送った案:
  - 仕様書に `FM_MEM_AMM = 5` を登録する。理由: 1 チップ 1 種類なので分ける意味が
    薄い。仕様書（別リポジトリ）の変更も要る
  - 仕様書の `FM_MEM_ADPCM_B_ROMMODE` を 5 に繰り下げ、`FM_MEM_AMM = 4` を登録する。
    理由: DSAemuEngine が `FM_MEM_ADPCM_B_ROMMODE = 4` を実装済みで、波及する
  - 4 をチップごとに読み替える（YMZ770 系なら AMM）。理由: 同じ番号が別のメモリを
    指すことになる

### 2.3 `FmEngine_SetMemoryEx`（任意）は見送り

- 決定（ユーザー）: 今回は実装しない。`FmEngine_SetMemory` だけでも仕様に準拠する
- 実装するときの論点（**確認済み(読解)**）:
  - PCMD8: `extern/ymz280b` は外部メモリを範囲検査なしの生ポインタ
    （`m_ext_mem[...]`）で直接読み書きする。割り当ての表を通すには、コアを `src/`
    にフォークしてメモリアクセスを関数経由にするか、無改変のまま ROM を 16MB の
    平坦なバッファへ複製し、RAM は base 0・16MB 以上の 1 ブロックだけ受けるかになる
  - AMM 部: `mpeg_audio` は ROM 先頭のポインタを持って読む。ROM は平坦なバッファへの
    複製で表せる。RAM をその場で読むには base 0 の 1 ブロックに限られる
  - SSGS / SSGS2 の ADPCM 部: 読み出しが `ymz_adpcm_device::rom_byte` を通るので、
    割り当ての表に置き換えやすい
- 呼び出し側は `GetProcAddress` で有無を確かめる約束なので、エクスポートしないことで
  壊れる呼び出し側は無い（仕様書の約束による。**確認済み(読解)**）

### 2.4 `FmEngine_GetNativeRate`

仕様書に「FM と SSG を別のレートで生成するチップ (OPN 系) では FM 部のレート」が
加わった。本エンジンのチップは FM 部を持たず OPN 系に当たらないので、変更しない
（SSGS3 は SSG 部のレート、AMMS-A はその時点の AMM の fs を返す。README のとおり）。

### 2.5 確認

**確認済み**（変更前の `63c8834` と変更後を同じ手順（VS 2026 の cl、Ninja、Release）で
ビルドし、同じ検査プログラムを両方の DLL に当てた。検査プログラムはリポジトリに
入れていない）:

- 変更後の DLL は 20 項目すべて通った。部位ゲインの 3 関数（9 チップ × 部位 0〜8、
  未知の `chip_id`、null 引数）、AMMS-A / SSGS3 の `FM_MEM_PCM` での `SetMemory` と
  `GetMemorySize`、種別 4 が `FM_ERR_UNAVAILABLE` になること、`SetMemoryEx` が
  エクスポートされていないこと
- 変更前の DLL では同じ検査のうち 7 項目が落ちた（部位ゲインのシンボルが無い、
  AMM 部が `FM_MEM_PCM` を受けない、種別 4 を受ける）。検査は変更の有無を見分けている
- 全 9 チップの出力（48kHz、9,600 サンプル、L/R）が変更前後でビット一致した。
  変更前は AMM ROM を種別 4、変更後は `FM_MEM_PCM` で渡した。どのチップも無音では
  ない（AMMS-A は乱数の ROM をデコードした出力で、RMS 0.23）
- エクスポートは必須 14 + 部位ゲイン 3 の 17 シンボル（`dumpbin /exports`）

**未検証**: FMEngineTest からの読み込みと再生、Linux / macOS でのビルド、本物の
AMM フレーズデータでの再生。
