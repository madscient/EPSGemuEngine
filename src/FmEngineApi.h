#pragma once
// FmEngineApi.h
// FmEngineApi の C インターフェース。互換エンジンと、エンジンを使う
// アプリケーションが共通で使う。
//
// 正本は https://github.com/madscient/FMEngineTest の include/FmEngineApi.h。
// 他のリポジトリにあるものは写しなので、直接編集しない。
// 仕様の詳細は同じリポジトリの docs/FmEngineApi.md を参照。
//
// チップ・部位・外部メモリはキーワード文字列で指定する ("OPNA"、"SSG"、
// "ADPCM_B" 等)。一覧はエンジンに問い合わせて取得するので、チップや部位や
// 外部メモリが増えてもこのヘッダは変わらない。

#include <stdint.h>

// ---- エクスポート属性 ---------------------------------------------------
#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef FMENGINE_EXPORTS
#    define FMENGINE_API __declspec(dllexport)
#  else
#    define FMENGINE_API __declspec(dllimport)
#  endif
#  define FMENGINE_CALL __cdecl
#else
#  if defined(FMENGINE_EXPORTS) && defined(__GNUC__)
#    define FMENGINE_API __attribute__((visibility("default")))
#  else
#    define FMENGINE_API
#  endif
#  define FMENGINE_CALL
#endif

// ---- 戻り値コード -------------------------------------------------------
typedef enum FmResult {
    FM_OK                =  0,
    FM_ERR_INVALID_ARG   = -1,
    FM_ERR_UNKNOWN_CHIP  = -2,  // FmEngine_AddChip で未知のチップ名
    FM_ERR_ALLOC         = -3,
    FM_ERR_UNAVAILABLE   = -4,
} FmResult;

// ---- 外部メモリにつないだデバイスの種類 ---------------------------------
typedef enum FmMemoryAccess {
    FM_ACCESS_ROM = 0,  // 割り当て中は内容が変わらない。エンジンは複製してよい。チップからの書き込みは捨てる
    FM_ACCESS_RAM = 1,  // チップ以外も書き換えてよい。エンジンは複製せず、その場で読み書きする
} FmMemoryAccess;

// ---- 不透明ハンドル -----------------------------------------------------
struct FmEngineOpaque;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FmEngineOpaque* FmEngineHandle;

// =========================================================
//  エンジン生成・破棄
// =========================================================
FMENGINE_API FmEngineHandle FMENGINE_CALL FmEngine_Create(uint32_t sample_rate);
FMENGINE_API void           FMENGINE_CALL FmEngine_Destroy(FmEngineHandle engine);

// =========================================================
//  対応チップ問い合わせ
//  FmEngine_Inquiry         : 対応チップの総数を返す。
//  FmEngine_GetSupportedChip: index 番目のチップ名を返す (範囲外は NULL)。
// =========================================================
FMENGINE_API uint32_t    FMENGINE_CALL FmEngine_Inquiry(FmEngineHandle engine);
FMENGINE_API const char* FMENGINE_CALL FmEngine_GetSupportedChip(
    FmEngineHandle engine, uint32_t index);

// =========================================================
//  チップ追加
//  name  : チップ名文字列 ("OPNA", "OPL2" 等、大文字小文字を区別する)
//  clock : マスタークロック Hz。エンジンは既定のクロックを持たないので、
//          0 は FM_ERR_INVALID_ARG。
//  未知の名前なら FM_ERR_UNKNOWN_CHIP を返す。
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL FmEngine_AddChip(
    FmEngineHandle engine, const char* name, uint32_t clock, uint32_t* out_id);

// =========================================================
//  チップ情報取得
// =========================================================
FMENGINE_API const char* FMENGINE_CALL FmEngine_GetChipName(
    FmEngineHandle engine, uint32_t chip_id);
// ネイティブサンプルレート (Hz、端数切り捨て)。
// FM と SSG を別のレートで生成するチップ (OPN 系) では FM 部のレート。
// OPN/OPNA では prescale レジスタ (0x2D-0x2F) の書き込みで変わる。
FMENGINE_API uint32_t    FMENGINE_CALL FmEngine_GetNativeRate(
    FmEngineHandle engine, uint32_t chip_id);
FMENGINE_API uint32_t    FMENGINE_CALL FmEngine_GetSampleRate(
    FmEngineHandle engine);

// =========================================================
//  レジスタ書き込み
//  port : OPL3/OPNA 等の bank/port 番号
//  スレッドセーフ: オーディオコールバックスレッドと並行して呼び出し可能。
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL FmEngine_Write(
    FmEngineHandle engine, uint32_t chip_id,
    uint8_t reg, uint8_t value, uint32_t port);

// =========================================================
//  ゲイン設定 (L/R 独立)
//  1.0 = 0 dB。オーディオコールバックスレッドと並行して呼び出し可能。
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL FmEngine_SetGain(
    FmEngineHandle engine, uint32_t chip_id, float gain_l, float gain_r);
FMENGINE_API FmResult FMENGINE_CALL FmEngine_GetGain(
    FmEngineHandle engine, uint32_t chip_id,
    float* out_gain_l, float* out_gain_r);

// =========================================================
//  部位ごとのゲイン (任意のエクスポート)
//  部位は、チップが別々の端子から出す出力。名前の文字列で指定する
//  (大文字小文字を区別する)。
//
//  この節の 4 関数は組でエクスポートする。呼び出し側は
//  FmEngine_GetPartCount の有無で判定し、無ければどれも呼ばない。
// =========================================================
// チップが持つ部位の数。部位を持たないチップと未知の chip_id は 0。
FMENGINE_API uint32_t    FMENGINE_CALL FmEngine_GetPartCount(
    FmEngineHandle engine, uint32_t chip_id);
// index 番目の部位の名前 (範囲外と未知の chip_id は NULL)。
// 文字列は FmEngine_Destroy が戻るまで有効。
FMENGINE_API const char* FMENGINE_CALL FmEngine_GetPartName(
    FmEngineHandle engine, uint32_t chip_id, uint32_t index);
// 実際に掛かるゲインは FmEngine_SetGain のゲイン × 部位のゲイン。
// 未知の chip_id、チップが持たない部位の名前、NULL は FM_ERR_INVALID_ARG。
// オーディオコールバックスレッドと並行して呼び出し可能。
FMENGINE_API FmResult FMENGINE_CALL FmEngine_SetPartGain(
    FmEngineHandle engine, uint32_t chip_id, const char* part,
    float gain_l, float gain_r);
FMENGINE_API FmResult FMENGINE_CALL FmEngine_GetPartGain(
    FmEngineHandle engine, uint32_t chip_id, const char* part,
    float* out_gain_l, float* out_gain_r);

// =========================================================
//  外部メモリ (任意のエクスポート)
//  チップが読み書きする、音源コアの外にあるメモリ (ADPCM の ROM や RAM 等)。
//  名前の文字列で指定する (大文字小文字を区別する)。
//
//  この節の 3 関数は組でエクスポートする。呼び出し側は
//  FmEngine_GetMemoryCount の有無で判定し、無ければ FmEngine_SetMemory も
//  FmEngine_SetMemoryEx も呼ばない。
// =========================================================
// チップが持つ外部メモリの数。持たないチップと未知の chip_id は 0。
FMENGINE_API uint32_t    FMENGINE_CALL FmEngine_GetMemoryCount(
    FmEngineHandle engine, uint32_t chip_id);
// index 番目の外部メモリの名前 (範囲外と未知の chip_id は NULL)。
// 文字列は FmEngine_Destroy が戻るまで有効。
FMENGINE_API const char* FMENGINE_CALL FmEngine_GetMemoryName(
    FmEngineHandle engine, uint32_t chip_id, uint32_t index);
// 未知の chip_id、チップが持たないメモリの名前、memory が NULL なら
// FM_ERR_INVALID_ARG。
// data の寿命は呼び出し元が管理すること。エンジンは data に書き込まない。
// エンジンが data を複製するか参照するかは、エンジンによる。
// オーディオストリーム開始前に呼ぶこと (スレッドセーフではない)。
FMENGINE_API FmResult    FMENGINE_CALL FmEngine_SetMemory(
    FmEngineHandle engine, uint32_t chip_id,
    const char* memory, const uint8_t* data, uint32_t size);

// =========================================================
//  外部メモリの割り当て (任意のエクスポート)
//  エクスポートするエンジンは、上の 3 関数もエクスポートする。
//
//  memory のメモリの [base, base + size) に data を割り当てる。
//  番地 base + i のバイトが data[i]。割り当ての無い番地を読むと 0、
//  書き込みは捨てる。data == NULL なら、その範囲と重なる割り当てを
//  すべて外す (access は無視)。
//  割り当てを外すか FmEngine_Destroy が戻るまで、data を解放しないこと。
//  オーディオストリーム開始前に呼ぶこと (スレッドセーフではない)。
//
//  戻り値:
//    FM_ERR_INVALID_ARG : 未知の chip_id、チップが持たないメモリの名前、
//                         memory が NULL、size が 0、既存の割り当てと
//                         範囲が重なる
//    FM_ERR_UNAVAILABLE : FM_ACCESS_RAM のブロックをその場で読み書きできない
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL FmEngine_SetMemoryEx(
    FmEngineHandle engine, uint32_t chip_id,
    const char* memory, uint32_t base,
    uint8_t* data, uint32_t size, FmMemoryAccess access);

// =========================================================
//  波形生成
//  out_l / out_r : float32 非インターリーブ、範囲 [-1.0, 1.0]
//  アプリケーションのオーディオコールバックから呼び出すこと。
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL FmEngine_Generate(
    FmEngineHandle engine, float* out_l, float* out_r, uint32_t samples);

#ifdef __cplusplus
} // extern "C"
#endif
