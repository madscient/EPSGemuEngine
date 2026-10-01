// EPSGemuEngine.cpp
// FmEngineApi 準拠エミュレーションエンジン
// 統合コア:
//   ay8910 (MAME 由来 / furnace fork)
//     → EPSG (AY8930) / SSG (YM2149) / PSG (AY-3-8910) / PSG2 (AY-3-8914)
//   YmzSsg (ay8910 の YM2149 を派生)
//     → SSGS (YMZ705) / SSGS2 (YMZ732) / SSGS3 (YMZ771) の SSG 互換部
//   Ymz770 (MAME 由来) + mpeg_audio (MAME 由来 / 無改変)
//     → SSGS3 (YMZ771) の AMM フレーズ再生・シーケンサ部
//   YmzAdpcm (YMZ280B のコーデックを流用)
//     → SSGS (YMZ705) / SSGS2 (YMZ732) の ADPCM 再生部
//   Ymz280b (MAME 由来 / furnace fork の ymz280b をフォーク)
//     → PCMD8 (YMZ280B)

#include "FmEngineApi.h"
#include "MemMap.h"
#include "YmzSsg.h"
#include "Ymz770.h"
#include "YmzAdpcm.h"
#include "Ymz280b.h"
#include "../extern/ay8910/ay8910.h"

#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <new>
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

// PCMD8 のコアは 8 ボイスの L/R を別々のバッファへ書き出す。
// レジスタ変更を遅らせないよう、まとめずに 1 サンプルずつ生成する
static constexpr uint32_t kPcmVoiceBufs = 16;

// AMM のフレーズテーブルが指せるアドレス空間 (24bit)。
// デコーダは読み出し上限をビット数の int で持つので、これより大きく渡さない
static constexpr uint32_t kAmmMemSpace = 1u << 24;

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
    PCMD8, // YMZ280B
};

// SSG を 2 系統持つ YMZ 系のチップか
static bool isSsgsFamily(ChipKind kind) {
    return kind == ChipKind::SSGS || kind == ChipKind::SSGS2 || kind == ChipKind::SSGS3;
}

// AMM フレーズ再生部を持つチップか
static bool hasAmm(ChipKind kind) {
    return kind == ChipKind::SSGS3 || kind == ChipKind::AMMSA;
}

// YMZ705 系の ADPCM 再生部を持つチップか
static bool hasAdpcm(ChipKind kind) {
    return kind == ChipKind::SSGS || kind == ChipKind::SSGS2;
}

// ay8910 コアをそのまま使うチップか
static bool isAyFamily(ChipKind kind) {
    return kind == ChipKind::EPSG || kind == ChipKind::SSG
        || kind == ChipKind::PSG  || kind == ChipKind::PSG2;
}

// =========================================================
//  低いレートのサンプル列を出力レートへ線形補間で伸ばす状態
// =========================================================
struct Interp {
    double step  = 0.0;   // 出力1サンプルあたりの入力サンプル数
    double phase = 1.0;   // prev と next の間の位相 (初回で next を読む)
    float  prev_l = 0.0f, prev_r = 0.0f;
    float  next_l = 0.0f, next_r = 0.0f;

    void set_rate(uint32_t src_rate, uint32_t dst_rate) {
        step = (double)src_rate / (double)dst_rate;
    }
};

// next は入力を 1 サンプル取り出す
template <typename NextFn>
static void interpCalc(Interp& ip, NextFn next,
                       float gain_l, float gain_r, float& out_l, float& out_r)
{
    ip.phase += ip.step;
    while (ip.phase >= 1.0) {
        ip.prev_l = ip.next_l;
        ip.prev_r = ip.next_r;
        next(ip.next_l, ip.next_r);
        ip.phase -= 1.0;
    }
    const float t = (float)ip.phase;
    out_l += (ip.prev_l + (ip.next_l - ip.prev_l) * t) * gain_l;
    out_r += (ip.prev_r + (ip.next_r - ip.prev_r) * t) * gain_r;
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
    std::shared_ptr<ay8910_device>      dev;    // EPSG / SSG / PSG / PSG2
    std::unique_ptr<ymz_ssg_chip>       ssgs;   // SSGS / SSGS2 / SSGS3 の SSG 部
    std::unique_ptr<ymz770_amm_device>  amm;    // SSGS3 / AMMS-A の AMM 部
    std::unique_ptr<ymz_adpcm_device>   adpcm;  // SSGS / SSGS2 の ADPCM 部
    std::unique_ptr<ymz280b_device>     pcm;    // PCMD8

    // native_rate → sample_rate のデシメーション状態
    double resample_step = 1.0;   // 出力1サンプルあたりの native ステップ数
    double tick_remain   = 0.0;   // 現在の native サンプルの未消費分
    float  tick_l        = 0.0f;  // 現在の native サンプル値 (L)
    float  tick_r        = 0.0f;  // 現在の native サンプル値 (R)
    float  dc_offset     = 0.0f;  // 全チャンネル無音時の出力レベル

    // 出力より低いレートで動くブロックの補間状態
    Interp   amm_interp;
    Interp   adpcm_interp;
    Interp   pcm_interp;
    uint32_t amm_rate = 0;        // AMM は fs が再生中に変わり得る

    // PCMD8 のコアが 8 ボイスの L/R を書き出す先
    int16_t pcm_voice_out[kPcmVoiceBufs] = {};

    // FM_MEM_PCM の外部メモリの割り当て。
    // AMM 部 / ADPCM 部 / PCMD8 を 2 つ以上持つチップは無いので 1 つで足りる
    MemMap mem;

    // PCMD8 の FmEngine_SetMemory で渡されたデータの複製。データは const だが、
    // チップは 0x87 で外部メモリに書き込むため、書き込める複製を割り当てる
    std::vector<uint8_t> mem_copy;

    // AMM のデコーダに渡す、割り当てを 1 本につないだ ROM の複製
    std::vector<uint8_t> amm_flat;

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
};

static const ChipDesc kChipTable[] = {
    { "EPSG",   ChipKind::EPSG  },  // AY8930
    { "SSG",    ChipKind::SSG   },  // YM2149
    { "PSG",    ChipKind::PSG   },  // AY-3-8910
    { "PSG2",   ChipKind::PSG2  },  // AY-3-8914
    { "SSGS",   ChipKind::SSGS  },  // YMZ705
    { "SSGS2",  ChipKind::SSGS2 },  // YMZ732
    { "SSGS3",  ChipKind::SSGS3 },  // YMZ771
    { "AMMS-A", ChipKind::AMMSA },  // YMZ770C
    { "PCMD8",  ChipKind::PCMD8 },  // YMZ280B
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
    case ChipKind::PCMD8: return clock / 384;
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

// PCMD8 のコアが外部メモリを読み書きする先 (context は ChipEntry::mem)
static uint8_t pcmMemRead(void* context, uint32_t address) {
    return static_cast<const MemMap*>(context)->read(address);
}
static void pcmMemWrite(void* context, uint32_t address, uint8_t data) {
    static_cast<const MemMap*>(context)->write(address, data);
}

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
    e->clock       = clock;
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

    if (hasAdpcm(desc.kind)) {
        e->adpcm = std::make_unique<ymz_adpcm_device>();
        if (!e->adpcm) return nullptr;
        e->adpcm->set_memory(&e->mem);
        e->adpcm_interp.set_rate(ymz_adpcm_device::kSampleRate, sample_rate);
    }

    if (desc.kind == ChipKind::PCMD8) {
        e->pcm = std::make_unique<ymz280b_device>();
        if (!e->pcm) return nullptr;
        e->pcm->device_start(pcmMemRead, pcmMemWrite, &e->mem);
        e->pcm->device_reset();
        e->pcm_interp.set_rate(e->native_rate, sample_rate);
    }

    if (hasAmm(desc.kind)) {
        e->amm = std::make_unique<ymz770_amm_device>(e->clock, kAmmDivider);
        if (!e->amm) return nullptr;
        // シーケンスコードは SSG 部のレジスタも書くため、
        // CPU からの書き込みと同じデコーダを経由させる
        e->amm->set_forward_write(forwardSequencerWrite, e.get());
        e->amm_rate = e->amm->sample_rate();
        e->amm_interp.set_rate(e->amm_rate, sample_rate);
    }

    if (isAyFamily(desc.kind)) {
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
    if (c.pcm) {
        // 割り当ての無い番地は 0 として読めるが、0 が続く 4bit ADPCM は
        // 無音にならないため、外部メモリが未設定の間はチップを動かさない
        if (c.mem.empty()) return;
        c.pcm->write(0, reg);
        c.pcm->write(1, val);
        return;
    }
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
        // SSGS / SSGS2 は $40-$B3 が ADPCM 部
        if (c.adpcm && reg >= 0x40 && reg <= 0xB3)
            c.adpcm->write(reg, val);
        else
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
//  fs は AMM のフレームヘッダで途中から変わり得る
// =========================================================
static void ammCalcStereo(ChipEntry& c, float& out_l, float& out_r) {
    if (c.amm->sample_rate() != c.amm_rate) {
        c.amm_rate = c.amm->sample_rate();
        c.amm_interp.set_rate(c.amm_rate, c.sample_rate);
    }
    interpCalc(c.amm_interp,
               [&c](float& l, float& r) { c.amm->step(l, r); },
               c.gain_l, c.gain_r, out_l, out_r);
}

// =========================================================
//  ADPCM 部 (SSGS / SSGS2) の 1 出力サンプル
// =========================================================
static void adpcmCalcStereo(ChipEntry& c, float& out_l, float& out_r) {
    interpCalc(c.adpcm_interp,
               [&c](float& l, float& r) { c.adpcm->step(l, r); },
               c.gain_l, c.gain_r, out_l, out_r);
}

// =========================================================
//  PCMD8 の 1 出力サンプル
//  コアは 8 ボイスの L/R を別バッファへ書くので、合成は自前で行う
// =========================================================
static void pcmCalcStereo(ChipEntry& c, float& out_l, float& out_r) {
    interpCalc(c.pcm_interp, [&c](float& l, float& r) {
        if (c.mem.empty()) { l = 0.0f; r = 0.0f; return; }

        int16_t* ptrs[kPcmVoiceBufs];
        for (uint32_t i = 0; i < kPcmVoiceBufs; ++i)
            ptrs[i] = &c.pcm_voice_out[i];
        c.pcm->sound_stream_update(ptrs, 1);

        int32_t acc_l = 0, acc_r = 0;
        for (uint32_t v = 0; v < kPcmVoiceBufs / 2; ++v) {
            acc_l += c.pcm_voice_out[v * 2];
            acc_r += c.pcm_voice_out[v * 2 + 1];
        }
        l = (float)acc_l / 32768.0f;
        r = (float)acc_r / 32768.0f;
    }, c.gain_l, c.gain_r, out_l, out_r);
}

// =========================================================
//  外部メモリの割り当て
// =========================================================
// FM_MEM_PCM の外部メモリを持つチップか
static bool hasExtMemory(const ChipEntry& c) {
    return c.amm || c.adpcm || c.pcm;
}

// AMM のデコーダは ROM 先頭のポインタから読むので、割り当てを 1 本の連続した
// 領域にして渡す。base 0 の 1 ブロックならそのまま渡し、それ以外は ROM を
// つないだ複製を作る。RAM はその場で読む約束なので、複製が要る形では受けられない
static FmResult ammView(const MemMap& map, std::vector<uint8_t>& flat,
                        const uint8_t*& data, uint32_t& size)
{
    std::vector<const MemMap::Block*> reach;
    for (const auto& b : map.blocks())
        if (b.base < kAmmMemSpace) reach.push_back(&b);

    data = nullptr;
    size = 0;
    if (reach.empty()) return FM_OK;
    if (reach.size() == 1 && reach[0]->base == 0) {
        data = reach[0]->read;
        size = std::min(reach[0]->size, kAmmMemSpace);
        return FM_OK;
    }

    uint64_t end = 0;
    for (const auto* b : reach) {
        if (b->write) return FM_ERR_UNAVAILABLE;
        end = std::max(end, (uint64_t)b->base + b->size);
    }
    size = (uint32_t)std::min<uint64_t>(end, kAmmMemSpace);
    flat.assign(size, 0);
    for (const auto* b : reach)
        memcpy(&flat[b->base], b->read, std::min(b->size, size - b->base));
    data = flat.data();
    return FM_OK;
}

// 外部メモリの割り当てを next に差し替え、その部をリセットする。
// AMM 部が受けられない割り当てなら、何も変えずに FM_ERR_UNAVAILABLE を返す
static FmResult applyMemory(ChipEntry& c, MemMap next)
{
    if (c.amm) {
        std::vector<uint8_t> flat;
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        const FmResult r = ammView(next, flat, data, size);
        if (r != FM_OK) return r;
        // data が flat を指していても、swap ではバッファが動かない
        c.amm_flat.swap(flat);
        c.amm->set_rom(data, size);
        // ROM 差し替えでレジスタもリセットされるため再生レートを取り直す
        c.amm_rate = c.amm->sample_rate();
        c.amm_interp.set_rate(c.amm_rate, c.sample_rate);
    }

    c.mem = std::move(next);
    if (c.adpcm) c.adpcm->set_memory(&c.mem);
    if (c.pcm)   c.pcm->device_reset();

    const bool copy_mapped = std::any_of(c.mem.blocks().begin(), c.mem.blocks().end(),
        [&c](const MemMap::Block& b) { return !c.mem_copy.empty() && b.read == c.mem_copy.data(); });
    if (!copy_mapped) std::vector<uint8_t>().swap(c.mem_copy);
    return FM_OK;
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
    // エンジンは既定のクロックを持たないので、0 を標準値に読み替えない
    if (!engine || !name || clock == 0) return FM_ERR_INVALID_ARG;
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

// 部位は仕様書の部位の表に載るチップだけが持つ。本エンジンのチップは
// 載っていないので、どのチップも部位を持たない
FMENGINE_API FmResult FMENGINE_CALL FmEngine_SetPartGain(
    FmEngineHandle /*engine*/, uint32_t /*chip_id*/, FmPart /*part*/,
    float /*gain_l*/, float /*gain_r*/)
{
    return FM_ERR_INVALID_ARG;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_GetPartGain(
    FmEngineHandle /*engine*/, uint32_t /*chip_id*/, FmPart /*part*/,
    float* /*out_gain_l*/, float* /*out_gain_r*/)
{
    return FM_ERR_INVALID_ARG;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_GetPartMask(
    FmEngineHandle engine, uint32_t chip_id, uint32_t* out_mask)
{
    if (!engine || chip_id >= engine->chips.size() || !out_mask) return FM_ERR_INVALID_ARG;
    *out_mask = 0;
    return FM_OK;
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_SetMemory(
    FmEngineHandle engine, uint32_t chip_id,
    FmMemoryType mem_type, const uint8_t* data, uint32_t size)
{
    if (!engine || chip_id >= engine->chips.size()) return FM_ERR_INVALID_ARG;
    ChipEntry& c = *engine->chips[chip_id];
    if (size != 0 && !data) return FM_ERR_INVALID_ARG;
    if (mem_type != FM_MEM_PCM || !hasExtMemory(c)) return FM_ERR_UNAVAILABLE;

    std::lock_guard<std::mutex> lock(engine->write_mutex);
    try {
        // それまでの割り当ては外し、[0, size) だけにする
        MemMap next;
        if (size != 0) {
            if (c.pcm) {
                std::vector<uint8_t> copy(data, data + size);
                next.add({ 0, size, copy.data(), copy.data() });
                c.mem_copy.swap(copy);
            } else {
                next.add({ 0, size, data, nullptr });
            }
        }
        return applyMemory(c, std::move(next));
    } catch (const std::bad_alloc&) {
        return FM_ERR_ALLOC;
    }
}

FMENGINE_API uint32_t FMENGINE_CALL FmEngine_GetMemorySize(
    FmEngineHandle engine, uint32_t chip_id, FmMemoryType mem_type)
{
    if (!engine || chip_id >= engine->chips.size()) return 0;
    if (mem_type != FM_MEM_PCM) return 0;
    return engine->chips[chip_id]->mem.total_size();
}

FMENGINE_API FmResult FMENGINE_CALL FmEngine_SetMemoryEx(
    FmEngineHandle engine, uint32_t chip_id,
    FmMemoryType mem_type, uint32_t base,
    uint8_t* data, uint32_t size, FmMemoryAccess access)
{
    if (!engine || chip_id >= engine->chips.size()) return FM_ERR_INVALID_ARG;
    ChipEntry& c = *engine->chips[chip_id];
    if (mem_type != FM_MEM_PCM || !hasExtMemory(c)) return FM_ERR_INVALID_ARG;
    if (!MemMap::valid_range(base, size)) return FM_ERR_INVALID_ARG;
    if (data && access != FM_ACCESS_ROM && access != FM_ACCESS_RAM) return FM_ERR_INVALID_ARG;

    std::lock_guard<std::mutex> lock(engine->write_mutex);
    try {
        MemMap next = c.mem;
        if (!data) {
            next.remove_overlapping(base, size);
            if (next.blocks().size() == c.mem.blocks().size()) return FM_OK;
        } else {
            if (next.overlaps_any(base, size)) return FM_ERR_INVALID_ARG;
            next.add({ base, size, data, access == FM_ACCESS_RAM ? data : nullptr });
        }
        return applyMemory(c, std::move(next));
    } catch (const std::bad_alloc&) {
        return FM_ERR_ALLOC;
    }
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
                if (chip->adpcm)             adpcmCalcStereo(*chip, l, r);
                if (chip->pcm)               pcmCalcStereo(*chip, l, r);
            }
        }
        out_l[i] = std::max(-1.0f, std::min(1.0f, l));
        out_r[i] = std::max(-1.0f, std::min(1.0f, r));
    }
    return FM_OK;
}

} // extern "C"
