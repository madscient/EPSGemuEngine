// EPSGemuEngine.cpp
// FmEngineApi 準拠エミュレーションエンジン
// 統合コア:
//   ay8910 (MAME 由来 / furnace fork)
//     → EPSG (AY8930) / SSG (YM2149) / PSG (AY-3-8910) / PSG2 (AY-3-8914)
//   YmzSsg (ay8910 の YM2149 を派生)
//     → SSGS (YMZ705) / SSGS2 (YMZ732) / SSGS3 (YMZ771) の SSG 互換部
//   Ymz770 (MAME 由来) + mpeg_audio (MAME 由来 / 無改変)
//     → SSGS3 (YMZ771) の AMM フレーズ再生・シーケンサ部

#include "FmEngineApi.h"
#include "YmzSsg.h"
#include "Ymz770.h"
#include "../extern/ay8910/ay8910.h"

#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>

// =========================================================
//  定数
// =========================================================
// 同一チップを扱う他エンジン (DSAemuEngine の SSG) と音量が揃う係数
static constexpr float kOutputScale = 1.0f / 65536.0f;

// マスタークロックから AMM 再生レートを得る分周比。
// データシートはリセット後の初期状態を「fs = XI / 512」と規定しており、
// 16.384MHz なら 32kHz になる。再生開始後は AMM のヘッダの fs に従う
static constexpr uint32_t kAmmDivider = 512;

// =========================================================
//  チップ種別列挙
// =========================================================
enum class ChipKind {
    EPSG,  // AY8930    (拡張モード対応)
    SSG,   // YM2149
    PSG,   // AY-3-8910
    PSG2,  // AY-3-8914 (レジスタ配置が異なる)
    SSGS,  // YMZ705    (YM2149 2 系統 + パンポット)
    SSGS2, // YMZ732    (YMZ705 とレジスタ互換。分周比のみ異なる)
    SSGS3, // YMZ771    (レジスタ配置が異なり、パンポットが 5bit)
    AMMSA, // YMZ770C   (AMM 部のみ。SSG を持たない)
};

// SSG を 2 系統持つ YMZ 系のチップか
static bool isSsgsFamily(ChipKind kind) {
    return kind == ChipKind::SSGS || kind == ChipKind::SSGS2 || kind == ChipKind::SSGS3;
}

// AMM フレーズ再生部を持つチップか
static bool hasAmm(ChipKind kind) {
    return kind == ChipKind::SSGS3 || kind == ChipKind::AMMSA;
}

// =========================================================
//  チップエントリ
// =========================================================
struct ChipEntry {
    ChipKind    kind;
    std::string name;
    uint32_t    sample_rate = 0;  // エンジンのサンプルレート
    uint32_t    clock       = 0;  // マスタークロック
    uint32_t    native_rate = 0;  // コアの内部ステップレート

    // コアの基底クラスに仮想デストラクタがないため、
    // 具象型のデストラクタを保持できる shared_ptr で持つ
    std::shared_ptr<ay8910_device>      dev;   // EPSG / SSG / PSG / PSG2
    std::unique_ptr<ymz_ssg_chip>       ssgs;  // SSGS / SSGS2 / SSGS3 の SSG 部
    std::unique_ptr<ymz770_amm_device>  amm;   // SSGS3 の AMM 部

    // native_rate → sample_rate のデシメーション状態
    double resample_step = 1.0;   // 出力1サンプルあたりの native ステップ数
    double tick_remain   = 0.0;   // 現在の native サンプルの未消費分
    float  tick_l        = 0.0f;  // 現在の native サンプル値 (L)
    float  tick_r        = 0.0f;  // 現在の native サンプル値 (R)
    float  dc_offset     = 0.0f;  // 全チャンネル無音時の出力レベル

    // AMM は SSG よりはるかに低いレートで動くため、専用に線形補間で伸ばす
    uint32_t amm_rate  = 0;
    double   amm_step  = 0.0;   // 出力1サンプルあたりの AMM サンプル数
    double   amm_phase = 1.0;   // prev と next の間の位相 (初回で next を読む)
    float    amm_prev_l = 0.0f, amm_prev_r = 0.0f;
    float    amm_next_l = 0.0f, amm_next_r = 0.0f;

    // AMM フレーズデータ ROM (寿命は呼び出し元が管理する)
    const uint8_t* amm_rom      = nullptr;
    uint32_t       amm_rom_size = 0;

    float gain_l = 1.0f;
    float gain_r = 1.0f;
};

// =========================================================
//  エンジン本体
// =========================================================
struct FmEngineOpaque {
    uint32_t sample_rate;
    std::vector<std::unique_ptr<ChipEntry>> chips;
    std::mutex write_mutex;
};

// =========================================================
//  対応チップテーブル
// =========================================================
struct ChipDesc {
    const char* name;
    ChipKind    kind;
    uint32_t    default_clock;
};

static const ChipDesc kChipTable[] = {
    { "EPSG", ChipKind::EPSG, 2000000 },  // AY8930
    { "SSG",  ChipKind::SSG,  2000000 },  // YM2149
    { "PSG",  ChipKind::PSG,  2000000 },  // AY-3-8910
    { "PSG2", ChipKind::PSG2, 2000000 },  // AY-3-8914
    { "SSGS",  ChipKind::SSGS,   4096000 },  // YMZ705
    { "SSGS2", ChipKind::SSGS2, 12288000 },  // YMZ732
    { "SSGS3", ChipKind::SSGS3, 16384000 },  // YMZ771
    { "AMMS-A", ChipKind::AMMSA, 16384000 }, // YMZ770C
};
static constexpr uint32_t kChipCount = (uint32_t)(sizeof(kChipTable) / sizeof(kChipTable[0]));

static const ChipDesc* findChipDesc(const char* name) {
    if (!name) return nullptr;
    for (uint32_t i = 0; i < kChipCount; ++i)
        if (strcmp(kChipTable[i].name, name) == 0)
            return &kChipTable[i];
    return nullptr;
}

// =========================================================
//  SSG ブロックの動作クロック
//  YMZ705 のマスタークロックは 4.096MHz か 6.144MHz の 2 択で、S6M ピンの
//  指定に応じて 1/2 または 1/3 に分周される。境界値は 2 つの規定値の中点を採る。
//  YMZ732 は 12.288MHz を 1/6 に分周する。
//  YMZ771 は SSG ブロックの動作クロックがデータシートに記載されていないが、
//  マスタークロック 16.384MHz の 1/8 がファミリ共通の 2.048MHz と一致する。
//  いずれも規定のマスタークロックでは 2.048MHz になる
// =========================================================
static uint32_t ssgsInternalClock(ChipKind kind, uint32_t master_clock) {
    if (kind == ChipKind::SSGS3) return master_clock / 8;
    if (kind == ChipKind::SSGS2) return master_clock / 6;
    return master_clock / ((master_clock >= 5120000) ? 3 : 2);
}

// =========================================================
//  コアの内部ステップレート
//  AY8930 はトーンカウンタを AY-3-8910 の 2 倍の速度で回すため
//  ステップレートも 2 倍になる (拡張モードの分解能に対応する)
//  SSG を持たない AMMS-A はフレーズ再生レートを返す
// =========================================================
static uint32_t nativeRate(ChipKind kind, uint32_t clock) {
    switch (kind) {
    case ChipKind::EPSG:  return clock / 4;
    case ChipKind::SSGS:
    case ChipKind::SSGS2:
    case ChipKind::SSGS3: return ssgsInternalClock(kind, clock) / 8;
    case ChipKind::AMMSA: return clock / kAmmDivider;
    default:              return clock / 8;
    }
}

// =========================================================
//  1 native ステップ生成 (DC 除去済み)
//  SSGS 系はチャンネルごとのパンポットを適用した L/R、
//  それ以外は 3ch を合成したモノラル値を L/R 双方に返す
// =========================================================
static void chipStep(ChipEntry& c, float& out_l, float& out_r) {
    if (c.ssgs) {
        out_l = 0.0f;
        out_r = 0.0f;
        c.ssgs->step(out_l, out_r);
        return;
    }
    short buf[ay8910_device::NUM_CHANNELS] = { 0, 0, 0 };
    c.dev->sound_stream_update(buf, 1);
    out_l = out_r = (float)(buf[0] + buf[1] + buf[2]) - c.dc_offset;
}

static void forwardSequencerWrite(void* context, uint8_t reg, uint8_t val);

// =========================================================
//  チップ生成
// =========================================================
static std::unique_ptr<ChipEntry> createChip(
    const ChipDesc& desc, uint32_t clock, uint32_t sample_rate)
{
    auto e = std::make_unique<ChipEntry>();
    e->kind        = desc.kind;
    e->name        = desc.name;
    e->sample_rate = sample_rate;
    e->clock       = (clock != 0) ? clock : desc.default_clock;
    e->native_rate = nativeRate(desc.kind, e->clock);
    if (e->native_rate == 0) return nullptr;

    if (isSsgsFamily(desc.kind)) {
        const uint32_t ssg_clock = ssgsInternalClock(desc.kind, e->clock);
        if (desc.kind == ChipKind::SSGS3)
            e->ssgs = std::make_unique<ymz771_ssg_device>(ssg_clock);
        else
            e->ssgs = std::make_unique<ymz705_device>(ssg_clock);
        if (!e->ssgs) return nullptr;
        e->ssgs->start();
    }

    if (hasAmm(desc.kind)) {
        e->amm = std::make_unique<ymz770_amm_device>(e->clock, kAmmDivider);
        if (!e->amm) return nullptr;
        // シーケンスコードは SSG 部のレジスタも書くため、
        // CPU からの書き込みと同じデコーダを経由させる
        e->amm->set_forward_write(forwardSequencerWrite, e.get());
        e->amm_rate = e->amm->sample_rate();
        e->amm_step = (double)e->amm_rate / (double)sample_rate;
    }

    if (!isSsgsFamily(desc.kind) && !hasAmm(desc.kind)) {
        switch (desc.kind) {
        case ChipKind::EPSG: e->dev = std::make_shared<ay8930_device>(e->native_rate); break;
        case ChipKind::SSG:  e->dev = std::make_shared<ym2149_device>(e->native_rate); break;
        case ChipKind::PSG:  e->dev = std::make_shared<ay8910_device>(e->native_rate); break;
        case ChipKind::PSG2: e->dev = std::make_shared<ay8914_device>(e->native_rate); break;
        default: break;
        }
        if (!e->dev) return nullptr;

        e->dev->device_start();
        e->dev->device_reset();

        // コアの出力は 0V を負値、電源電圧側を正値で表す片極性信号なので、
        // リセット直後 (全チャンネル無音) のレベルを DC 成分として控えておく
        short buf[ay8910_device::NUM_CHANNELS] = { 0, 0, 0 };
        e->dev->sound_stream_update(buf, 1);
        e->dc_offset = (float)(buf[0] + buf[1] + buf[2]);
    }

    e->resample_step = (double)e->native_rate / (double)sample_rate;

    return e;
}

// =========================================================
//  レジスタ書き込み
//  アドレスラッチの上位ニブルが 0 以外ならチップが非選択になる
//  実チップの挙動をそのまま踏襲する
//  AY-3-8914 はレジスタ配置が異なるため専用ハンドラを通す
// =========================================================
static void chipWrite(ChipEntry& c, uint8_t reg, uint8_t val);

// シーケンサからのレジスタ書き込みを CPU からの書き込みと同じ経路へ流す
static void forwardSequencerWrite(void* context, uint8_t reg, uint8_t val) {
    chipWrite(*static_cast<ChipEntry*>(context), reg, val);
}

static void chipWrite(ChipEntry& c, uint8_t reg, uint8_t val) {
    if (c.amm) {
        // SSGS3 は $10-$32 が SSG 部、それ以外が AMM 部のレジスタ。
        // SSG を持たない AMMS-A はすべて AMM 部へ渡す
        if (c.ssgs && reg >= 0x10 && reg <= 0x32)
            c.ssgs->write(reg, val);
        else
            c.amm->write(reg, val);
        return;
    }
    if (c.ssgs) {
        c.ssgs->write(reg, val);
        return;
    }
    if (c.kind == ChipKind::PSG2) {
        static_cast<ay8914_device*>(c.dev.get())->write(reg, val);
        return;
    }
    c.dev->address_w(reg);
    c.dev->data_w(val);
}

// =========================================================
//  1 出力サンプル生成
//  native_rate から sample_rate への区間平均 (integrate & dump)
// =========================================================
static void chipCalcStereo(ChipEntry& c, float& out_l, float& out_r) {
    double need  = c.resample_step;
    double acc_l = 0.0;
    double acc_r = 0.0;
    while (need > 0.0) {
        if (c.tick_remain <= 0.0) {
            chipStep(c, c.tick_l, c.tick_r);
            c.tick_remain = 1.0;
        }
        const double take = std::min(need, c.tick_remain);
        acc_l        += (double)c.tick_l * take;
        acc_r        += (double)c.tick_r * take;
        c.tick_remain -= take;
        need          -= take;
    }
    out_l += (float)(acc_l / c.resample_step) * kOutputScale * c.gain_l;
    out_r += (float)(acc_r / c.resample_step) * kOutputScale * c.gain_r;
}

// =========================================================
//  AMM 部の 1 出力サンプル
//  再生レートは出力より低く、しかも AMM のフレームヘッダで途中から
//  変わり得るため、区間平均ではなく線形補間で伸ばす
// =========================================================
static void ammCalcStereo(ChipEntry& c, float& out_l, float& out_r) {
    if (c.amm->sample_rate() != c.amm_rate) {
        c.amm_rate = c.amm->sample_rate();
        c.amm_step = (double)c.amm_rate / (double)c.sample_rate;
    }

    c.amm_phase += c.amm_step;
    while (c.amm_phase >= 1.0) {
        c.amm_prev_l = c.amm_next_l;
        c.amm_prev_r = c.amm_next_r;
        c.amm->step(c.amm_next_l, c.amm_next_r);
        c.amm_phase -= 1.0;
    }

    const float t = (float)c.amm_phase;
    out_l += (c.amm_prev_l + (c.amm_next_l - c.amm_prev_l) * t) * c.gain_l;
    out_r += (c.amm_prev_r + (c.amm_next_r - c.amm_prev_r) * t) * c.gain_r;
}

// =========================================================
//  C API 実装
// =========================================================
extern "C" {

FMENGINE_API FmEngineHandle FMENGINE_CALL FmEngine_Create(uint32_t sample_rate) {
    if (sample_rate == 0) sample_rate = 48000;
    auto* eng = new(std::nothrow) FmEngineOpaque();
    if (!eng) return nullptr;
    eng->sample_rate = sample_rate;
    return eng;
}

FMENGINE_API void FMENGINE_CALL FmEngine_Destroy(FmEngineHandle engine) {
    delete engine;
}

FMENGINE_API uint32_t FMENGINE_CALL FmEngine_Inquiry(FmEngineHandle /*engine*/) {
    return kChipCount;
}

FMENGINE_API const char* FMENGINE_CALL FmEngine_GetSupportedChip(
    FmEngineHandle /*engine*/, uint32_t index)
{
    if (index >= kChipCount) return nullptr;
    return kChipTable[index].name;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_AddChip(
    FmEngineHandle engine, const char* name, uint32_t clock, uint32_t* out_id)
{
    if (!engine || !name) return FM_ERR_INVALID_ARG;
    const ChipDesc* desc = findChipDesc(name);
    if (!desc) return FM_ERR_UNKNOWN_CHIP;

    auto chip = createChip(*desc, clock, engine->sample_rate);
    if (!chip) return FM_ERR_ALLOC;

    if (out_id) *out_id = (uint32_t)engine->chips.size();
    engine->chips.push_back(std::move(chip));
    return FM_OK;
}

FMENGINE_API const char* FMENGINE_CALL FmEngine_GetChipName(
    FmEngineHandle engine, uint32_t chip_id)
{
    if (!engine || chip_id >= engine->chips.size()) return nullptr;
    return engine->chips[chip_id]->name.c_str();
}

FMENGINE_API uint32_t FMENGINE_CALL FmEngine_GetNativeRate(
    FmEngineHandle engine, uint32_t chip_id)
{
    if (!engine || chip_id >= engine->chips.size()) return 0;
    return engine->chips[chip_id]->native_rate;
}

FMENGINE_API uint32_t FMENGINE_CALL FmEngine_GetSampleRate(FmEngineHandle engine) {
    if (!engine) return 0;
    return engine->sample_rate;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_Write(
    FmEngineHandle engine, uint32_t chip_id,
    uint8_t reg, uint8_t value, uint32_t /*port*/)
{
    if (!engine || chip_id >= engine->chips.size()) return FM_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(engine->write_mutex);
    chipWrite(*engine->chips[chip_id], reg, value);
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_SetGain(
    FmEngineHandle engine, uint32_t chip_id, float gain_l, float gain_r)
{
    if (!engine || chip_id >= engine->chips.size()) return FM_ERR_INVALID_ARG;
    engine->chips[chip_id]->gain_l = gain_l;
    engine->chips[chip_id]->gain_r = gain_r;
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_GetGain(
    FmEngineHandle engine, uint32_t chip_id,
    float* out_gain_l, float* out_gain_r)
{
    if (!engine || chip_id >= engine->chips.size()) return FM_ERR_INVALID_ARG;
    if (out_gain_l) *out_gain_l = engine->chips[chip_id]->gain_l;
    if (out_gain_r) *out_gain_r = engine->chips[chip_id]->gain_r;
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_SetMemory(
    FmEngineHandle engine, uint32_t chip_id,
    FmMemoryType mem_type, const uint8_t* data, uint32_t size)
{
    if (!engine || chip_id >= engine->chips.size()) return FM_ERR_INVALID_ARG;
    ChipEntry& c = *engine->chips[chip_id];
    if (mem_type != FM_MEM_AMM || !c.amm) return FM_ERR_UNAVAILABLE;
    if (size != 0 && !data) return FM_ERR_INVALID_ARG;

    std::lock_guard<std::mutex> lock(engine->write_mutex);
    c.amm_rom      = (size != 0) ? data : nullptr;
    c.amm_rom_size = (data != nullptr) ? size : 0;
    c.amm->set_rom(c.amm_rom, c.amm_rom_size);

    // ROM 差し替えでレジスタもリセットされるため再生レートを取り直す
    c.amm_rate = c.amm->sample_rate();
    c.amm_step = (double)c.amm_rate / (double)c.sample_rate;
    return FM_OK;
}

FMENGINE_API uint32_t FMENGINE_CALL FmEngine_GetMemorySize(
    FmEngineHandle engine, uint32_t chip_id, FmMemoryType mem_type)
{
    if (!engine || chip_id >= engine->chips.size()) return 0;
    const ChipEntry& c = *engine->chips[chip_id];
    return (mem_type == FM_MEM_AMM) ? c.amm_rom_size : 0;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_Generate(
    FmEngineHandle engine, float* out_l, float* out_r, uint32_t samples)
{
    if (!engine || !out_l || !out_r) return FM_ERR_INVALID_ARG;

    for (uint32_t i = 0; i < samples; ++i) {
        float l = 0.0f, r = 0.0f;
        {
            std::lock_guard<std::mutex> lock(engine->write_mutex);
            for (auto& chip : engine->chips) {
                if (chip->ssgs || chip->dev) chipCalcStereo(*chip, l, r);
                if (chip->amm)               ammCalcStereo(*chip, l, r);
            }
        }
        out_l[i] = std::max(-1.0f, std::min(1.0f, l));
        out_r[i] = std::max(-1.0f, std::min(1.0f, r));
    }
    return FM_OK;
}

} // extern "C"
