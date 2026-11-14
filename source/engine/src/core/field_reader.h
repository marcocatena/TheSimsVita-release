/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * http://mozilla.org/MPL/2.0/.
 *
 * Ported from FreeSims (IffFieldEncode.cs).
 */

// Reads Maxis' bit-packed integer fields (TTAB versions 9-10, ObjM, ...): each
// field is a presence bit, a 2-bit width code, then a signed big-endian value.
#pragma once

#include <cstdint>

#include "core/bytes.h"

namespace ts {

class FieldReader {
public:
    explicit FieldReader(ByteReader &r) : r_(r) { byte_ = r_.u8(); }

    uint16_t u16() { return uint16_t(field(kNarrow)); }
    int16_t s16() { return int16_t(field(kNarrow)); }
    uint32_t u32() { return uint32_t(field(kWide)); }
    int32_t s32() { return int32_t(field(kWide)); }
    uint8_t u8() { return uint8_t(field(kByte)); }

    // True once every byte of the input has been consumed.
    bool at_end() const { return exhausted_; }

private:
    static constexpr uint8_t kNarrow[4] = { 5, 8, 13, 16 };
    static constexpr uint8_t kWide[4] = { 6, 11, 21, 32 };
    static constexpr uint8_t kByte[4] = { 2, 4, 6, 8 };

    int64_t field(const uint8_t (&widths)[4])
    {
        if (!bit())
            return 0;
        int width = widths[bits(2)];
        int64_t v = bits(width);
        if (v & (int64_t(1) << (width - 1)))
            v -= int64_t(1) << width;   // sign-extend
        return v;
    }

    uint32_t bits(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; i++)
            v = v << 1 | bit();
        return v;
    }

    uint32_t bit()
    {
        uint32_t b = (byte_ >> (7 - bit_pos_)) & 1;
        if (++bit_pos_ > 7) {
            bit_pos_ = 0;
            if (r_.remaining()) {
                byte_ = r_.u8();
            } else {
                byte_ = 0;   // past the end reads as zeros
                exhausted_ = true;
            }
        }
        return b;
    }

    ByteReader &r_;
    uint8_t byte_ = 0;
    int bit_pos_ = 0;
    bool exhausted_ = false;
};

} // namespace ts
