#pragma once
// FmEngineApi.h
// FmEngine の C API ファサード。
// このヘッダだけを include すれば DLL を利用できる。
// ymfm / FmChip.h 等の内部ヘッダへの依存はない。
//
// チップはキーワード文字列で指定する ("OPNA", "OPL2" 等)。
// 対応チップの一覧は FmEngine_Inquiry / FmEngine_GetSupportedChip で取得できる。
// 新しいチップが追加されてもこのヘッダを変更する必要はない。

#include <cstdint>

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
    FM_ERR_UNKNOWN_CHIP  = -2,  // 未知のチップ名
    FM_ERR_ALLOC         = -3,
    FM_ERR_UNAVAILABLE   = -4,
} FmResult;

// ---- メモリ種別 ---------------------------------------------------------
typedef enum FmMemoryType {
    FM_MEM_ADPCM_A = 1,  // ADPCM-A ROM (OPNA/OPNB/OPNBB)
    FM_MEM_ADPCM_B = 2,  // ADPCM-B ROM/RAM (OPNA/OPNB/OPNBB/Y8950)
    FM_MEM_PCM     = 3,  // PCM ROM (OPL4)
} FmMemoryType;

// ---- 出力の部位 ---------------------------------------------------------
// チップが別々の端子から出す出力。番号はチップをまたいで重ならない。
// 出力が1本のチップ (OPL/OPL2/Y8950/OPN2/OPM/OPZ) は部位を持たない。
typedef enum FmPart {
    FM_PART_OPN_FM      = 0,  // OPN/OPNA/OPNB/OPNBB: FM 部 (ADPCM・リズムを含む)
    FM_PART_OPN_SSG     = 1,  //   SSG 部
    FM_PART_OPLL_MELODY = 2,  // OPLL/OPLLP/OPLLX/VRC7: メロディ
    FM_PART_OPLL_RHYTHM = 3,  //   リズム
    FM_PART_OPL3_AB     = 4,  // OPL3: 出力 A (L) / B (R)
    FM_PART_OPL3_CD     = 5,  //   出力 C (L) / D (R)。既定のゲインは 0
    FM_PART_OPL4_DO0    = 6,  // OPL4: DO0 (FM の C/D)。既定のゲインは 0
    FM_PART_OPL4_DO1    = 7,  //   DO1 (AWM の C/D)。既定のゲインは 0
    FM_PART_OPL4_DO2    = 8,  //   DO2 (FM の A/B と AWM の A/B のミックス)
} FmPart;

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
//  チップはキーワード文字列で識別される ("OPNA", "OPL2", "OPM" 等)。
//  FmEngine_Inquiry       : 対応チップの総数を返す。
//  FmEngine_GetSupportedChip: index 番目のチップ名を返す (範囲外は nullptr)。
// =========================================================
FMENGINE_API uint32_t    FMENGINE_CALL FmEngine_Inquiry(FmEngineHandle engine);
FMENGINE_API const char* FMENGINE_CALL FmEngine_GetSupportedChip(
    FmEngineHandle engine, uint32_t index);

// =========================================================
//  チップ追加
//  name  : チップ名文字列 ("OPNA", "OPL2" 等、大文字小文字を区別する)
//  clock : マスタークロック Hz。0 で各チップの標準クロックを使用。
//  未知の名前なら FM_ERR_UNKNOWN_CHIP を返す。
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL FmEngine_AddChip(
    FmEngineHandle engine, const char* name, uint32_t clock, uint32_t* out_id);

// =========================================================
//  チップ情報取得
// =========================================================
FMENGINE_API const char* FMENGINE_CALL FmEngine_GetChipName(
    FmEngineHandle engine, uint32_t chip_id);
// FM 部のネイティブサンプルレート (Hz、端数切り捨て)。
// OPN/OPNA では prescale レジスタ (0x2D-0x2F) の書き込みで変わる。
FMENGINE_API uint32_t    FMENGINE_CALL FmEngine_GetNativeRate(
    FmEngineHandle engine, uint32_t chip_id);
FMENGINE_API uint32_t    FMENGINE_CALL FmEngine_GetSampleRate(
    FmEngineHandle engine);

// =========================================================
//  レジスタ書き込み
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
//  部位ごとのゲイン設定 (L/R 独立)
//  実際に掛かるゲインは FmEngine_SetGain のゲイン × 部位のゲイン。
//  既定値は 1.0 (FM_PART_OPL3_CD / FM_PART_OPL4_DO0 / FM_PART_OPL4_DO1 は 0)。
//  チップが持たない部位を指定すると FM_ERR_INVALID_ARG。
//  オーディオコールバックスレッドと並行して呼び出し可能。
// =========================================================
FMENGINE_API FmResult FMENGINE_CALL FmEngine_SetPartGain(
    FmEngineHandle engine, uint32_t chip_id, FmPart part,
    float gain_l, float gain_r);
FMENGINE_API FmResult FMENGINE_CALL FmEngine_GetPartGain(
    FmEngineHandle engine, uint32_t chip_id, FmPart part,
    float* out_gain_l, float* out_gain_r);
// チップが持つ部位をビットマスクで返す (bit n = FmPart の n 番)。
// 部位を持たないチップは 0。未知の chip_id なら FM_ERR_INVALID_ARG。
FMENGINE_API FmResult FMENGINE_CALL FmEngine_GetPartMask(
    FmEngineHandle engine, uint32_t chip_id, uint32_t* out_mask);

// =========================================================
//  外部メモリ設定 (ADPCM/PCM ROM/RAM)
//  data の寿命は呼び出し元が管理すること。
//  オーディオストリーム開始前に呼ぶこと (スレッドセーフではない)。
// =========================================================
FMENGINE_API FmResult  FMENGINE_CALL FmEngine_SetMemory(
    FmEngineHandle engine, uint32_t chip_id,
    FmMemoryType mem_type, const uint8_t* data, uint32_t size);
FMENGINE_API uint32_t  FMENGINE_CALL FmEngine_GetMemorySize(
    FmEngineHandle engine, uint32_t chip_id, FmMemoryType mem_type);

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
