#pragma once
// MemMap.h
// 外部メモリの番地の範囲ごとに、呼び出し側のブロックを割り当てる表。
// 割り当ての無い番地を読むと 0、書き込みは捨てる。ROM のブロックへの書き込みも捨てる。

#include <cstdint>
#include <vector>

class MemMap
{
public:
    struct Block {
        uint32_t       base;
        uint32_t       size;
        const uint8_t* read;
        uint8_t*       write;  // ROM なら nullptr
    };

    // 範囲は 64bit で比べ、base + size が 2^32 ちょうどの範囲も表せるようにする
    static bool valid_range(uint32_t base, uint32_t size) {
        return size > 0 && uint64_t{base} + size <= (uint64_t{1} << 32);
    }

    static bool overlaps(const Block& b, uint32_t base, uint32_t size) {
        return uint64_t{base} < uint64_t{b.base} + b.size
            && uint64_t{b.base} < uint64_t{base} + size;
    }

    bool overlaps_any(uint32_t base, uint32_t size) const {
        for (const Block& b : m_blocks)
            if (overlaps(b, base, size)) return true;
        return false;
    }

    void add(const Block& b) { m_blocks.push_back(b); }

    void remove_overlapping(uint32_t base, uint32_t size) {
        std::vector<Block> kept;
        for (const Block& b : m_blocks)
            if (!overlaps(b, base, size)) kept.push_back(b);
        m_blocks.swap(kept);
    }

    void clear() { m_blocks.clear(); }
    bool empty() const { return m_blocks.empty(); }
    const std::vector<Block>& blocks() const { return m_blocks; }

    uint8_t read(uint32_t address) const {
        if (const Block* b = find(address)) return b->read[address - b->base];
        return 0;
    }

    void write(uint32_t address, uint8_t data) const {
        if (const Block* b = find(address))
            if (b->write) b->write[address - b->base] = data;
    }

private:
    const Block* find(uint32_t address) const {
        for (const Block& b : m_blocks)
            if (address >= b.base && address - b.base < b.size) return &b;
        return nullptr;
    }

    std::vector<Block> m_blocks;
};
