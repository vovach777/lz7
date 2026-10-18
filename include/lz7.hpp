// https://godbolt.org/z/1fd69Ez7W
#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define _USE_SIMD
#define MAX_OFFSET ((1 << 17) - 1)
#define NO_MATCH_OFS (MAX_OFFSET)
#define ENCODE_MIN (4)
// Глубина поиска по цепочке: жёсткий потолок и выход после MISS_STREAK кандидатов подряд
// без улучшения. Дефолты подобраны по статистике Calgary/sqlite3.c/libstdc++:
// лучший матч в 83-90% случаев находится за первые 8 кандидатов.
#ifndef MAX_ATTEMPTS
#define MAX_ATTEMPTS (256)
#endif
#ifndef MISS_STREAK
#define MISS_STREAK (8)
#endif
#define HASH_LOG2 (16)
#define HASH_SIZE (1 << HASH_LOG2)
#define CHAIN_LOG2 (16)
#define CHAIN_SIZE (1 << CHAIN_LOG2)
#define CHAIN_BREAK (CHAIN_SIZE - 1)
// #define LOOK_AHEAD (2)
// #define RLE_INDEX_TRIGGER 10
// #define RLE_INDEX_DISTANCE 0x1000
// #define REP2_INDEX_DISTANCE 0x1000
// #define RLE_ACCEPTABLE_GAIN 0x1000
// #define BEST_ACCEPTABLE_GAIN 8
// #define LOOK_AHEAD_ACCEPTABLE_GAIN 0x1000
// // #define _USE_FAST_SKIP
// #define _USE_JUMP_OVER_MATCH 0
// // #define _ENABLE_RLE
// #define CHECK_IP_END (sizeof(uint32_t))
#if CHAIN_LOG2 > 16
#error "CHAIN_LOG2 > chain is uint16_t only"
#endif
#if HASH_LOG2 > 16
#error "HASH_LOG2 > 16: hashes are uint16_t (and _mm_packus_epi32 saturates)"
#endif
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <array>
#include <optional>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace lz7
{

    using Put_function = std::function<void(int offset, int len, const uint8_t *literals, int literals_len)>;

    inline bool is_rle(const uint8_t *r)
    {

        return std::memcmp(r, r + 1, ENCODE_MIN - 1) == 0;
    }

    inline bool is_rep2(const uint8_t *r)
    {

        return std::memcmp(r, r + 2, 2) == 0;
    }

    inline int ilog2(unsigned v)
    {
        int targetlevel = 1;
        while (v >>= 1)
            ++targetlevel;
        return targetlevel;
    }

    template <int inc = 1, int above = 0, int set = 0>
    inline int inc_above_or_set(int value)
    {
        return value > above ? value + inc : set;
    }

    // Невыровненная загрузка 32 бит без UB (компилируется в одну mov/ldr).
    inline uint32_t load_u32(const uint8_t *p)
    {
        uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    inline uint32_t match_of(const uint8_t *it)
    {
#if ENCODE_MIN == 3
        return (load_u32(it) & 0xffffff);
#else
        return load_u32(it);
#endif
    }

    inline uint16_t hash_of(const uint8_t *it)
    {
        return static_cast<uint16_t>(match_of(it) * 2654435761u >> (32 - HASH_LOG2));
    }

    // Хэш для 4 соседних позиций: hashes[i] = hash_of(it + i).
    // SSE4.1 / NEON / скалярный fallback; результат бит-в-бит одинаковый.
    inline void hash_of_sse42(const uint8_t *it, uint16_t *hashes)
    {
#if defined(__SSE4_1__)
        __m128i data = _mm_set_epi32(
            (int)load_u32(it + 3),
            (int)load_u32(it + 2),
            (int)load_u32(it + 1),
            (int)load_u32(it + 0));
        __m128i multiplier = _mm_set1_epi32(2654435761u);
        __m128i hash_values = _mm_mullo_epi32(data, multiplier);
        __m128i shifted = _mm_srli_epi32(hash_values, 32 - HASH_LOG2);
        __m128i result = _mm_packus_epi32(shifted, shifted);
        _mm_storel_epi64(reinterpret_cast<__m128i *>(hashes), result);
#elif defined(__ARM_NEON)
        const uint32_t lanes[4] = {load_u32(it), load_u32(it + 1), load_u32(it + 2), load_u32(it + 3)};
        uint32x4_t data = vld1q_u32(lanes);
        uint32x4_t hash_values = vmulq_n_u32(data, 2654435761u);
        uint32x4_t shifted = vshrq_n_u32(hash_values, 32 - HASH_LOG2);
        vst1_u16(hashes, vmovn_u32(shifted));
#else
        for (int i = 0; i < 4; ++i)
            hashes[i] = static_cast<uint16_t>(load_u32(it + i) * 2654435761u >> (32 - HASH_LOG2));
#endif
    }

    // Все 16 байт равны? (портабельно: без movemask)
#if defined(__ARM_NEON)
    inline bool neon_all_eq16(uint8x16_t eq)
    {
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 1);
        return (lo & hi) == ~0ULL;
    }
#endif

    inline int match_len_simd(const uint8_t *a, const uint8_t *a_end, const uint8_t *b, const uint8_t *b_end)
    {
        int len = 0;
#if defined(_USE_SIMD) && defined(__SSE2__)
        while (a + 16 <= a_end && b + 16 <= b_end)
        {
            __m128i va = _mm_loadu_si128((__m128i *)a);
            __m128i vb = _mm_loadu_si128((__m128i *)b);
            __m128i cmp = _mm_cmpeq_epi8(va, vb);
            int mask = _mm_movemask_epi8(cmp);
            if (mask != 0xFFFF)
            {
                return len + __builtin_ctz(~mask); // первая разница
            }
            len += 16;
            a += 16;
            b += 16;
        }
#elif defined(_USE_SIMD) && defined(__ARM_NEON)
        while (a + 16 <= a_end && b + 16 <= b_end)
        {
            uint8x16_t eq = vceqq_u8(vld1q_u8(a), vld1q_u8(b));
            if (!neon_all_eq16(eq))
            {
                // первая разница внутри этого блока: досканировать побайтово
                while (a < a_end && b < b_end && *a == *b) { ++a; ++b; ++len; }
                return len;
            }
            len += 16;
            a += 16;
            b += 16;
        }
#endif
        // добить хвост
        while (a < a_end && b < b_end && *a == *b)
        {
            ++a;
            ++b;
            ++len;
        }
        return len;
    }

    inline int match_len_simd_backward(const uint8_t *a_begin, const uint8_t *a, const uint8_t *b_begin, const uint8_t *b)
    {
        int len = 0;
#if defined(_USE_SIMD) && defined(__ARM_NEON)
        while (a - 15 >= a_begin && b - 15 >= b_begin)
        {
            uint8x16_t eq = vceqq_u8(vld1q_u8(a - 15), vld1q_u8(b - 15));
            if (!neon_all_eq16(eq))
            {
                while (a >= a_begin && b >= b_begin && *a == *b) { --a; --b; ++len; }
                return len;
            }
            len += 16;
            a -= 16;
            b -= 16;
        }
#elif defined(_USE_SIMD) && defined(__SSE2__)
        while (a - 15 >= a_begin && b - 15 >= b_begin)
        {
            __m128i va = _mm_loadu_si128((const __m128i *)(a - 15));
            __m128i vb = _mm_loadu_si128((const __m128i *)(b - 15));
            __m128i cmp = _mm_cmpeq_epi8(va, vb);
            int mask = _mm_movemask_epi8(cmp);

            if (mask != 0xFFFF)
            {
                // Ищем первую единицу с конца mask (0 = несовпадение)
                // Развернём маску для использования __builtin_clz
                uint32_t inv_mask = (~mask) & 0xFFFF;
                int leading_zeros = __builtin_clz(inv_mask) - 16; // mask в младших 16 битах
                return len + leading_zeros;
            }

            len += 16;
            a -= 16;
            b -= 16;
        }
#endif

        while (a >= a_begin && b >= b_begin && *a == *b)
        {
            --a;
            --b;
            ++len;
        }

        return len;
    }

    struct Best
    {
        const uint8_t *ofs{};
        const uint8_t *ip2{};
        int len{};
        int gain{std::numeric_limits<int>::min()};

        constexpr operator bool() const {
            return len >= ENCODE_MIN;
        }

        constexpr int test_ofs() const
        {
            return std::distance(ofs, ip2);
        }
        constexpr auto test_short() const
        {
            return test_ofs() < (1 << 10);
        }
        constexpr auto is_short(const uint8_t *emitp) const
        {
            auto literal_len = std::distance(emitp, ip2);
            return len >= ENCODE_MIN && test_short() && (literal_len <= 3);
        }

        void set_to_literals(const uint8_t *emitp)
        {
            auto literal_len = std::distance(emitp, ip2);
            len = 0;
            gain = -2 /*cost*/ - literal_len - (literal_len + 255 - 31) / 255;
        }

        void optimize(const uint8_t *emitp)
        {
            assert(ip2 >= emitp);
            if (len < ENCODE_MIN || test_ofs() > MAX_OFFSET)
            {
                set_to_literals(emitp);
                return;
            }
            if (len == 7 + ENCODE_MIN)
            {
                len--; //  std::cerr << "optimzation len==11 is useless" << std::endl;
            }
            assert(test_ofs() > 0);
            auto literal_len = std::distance(emitp, ip2);
            assert(literal_len >= 0);
            // euristic optimization
            int cost;
            if (test_short() && literal_len <= 3)
            {
                cost = 2 + match_cost(len);
            }
            else
                cost = 3 + literal_cost(literal_len) + match_cost(len);
            gain = len - cost - literal_len;

            // add a little bit gain to keep literals in range 3
            if (test_short() && literal_len <= 3)
                gain += 1;
        }

        constexpr int literal_cost(unsigned long nlit)
        {
            return (nlit + 255 - 7) / 255;
        }

        constexpr int match_cost(unsigned long len)
        {
            return (len + 255 - ENCODE_MIN - 7) / 255;
        }
    };

    class TokenSearcher
    {
        struct HashItem
        {
            const uint8_t *idx{nullptr};
            uint16_t next_item{CHAIN_BREAK};
            // uint16_t offset_relative{0}; //is allways 0. idx = idx-0;
        };
        struct ChainItem
        {
            uint16_t next_item{CHAIN_BREAK};
            uint16_t offset_relative{0xFFFF};
        };
        std::vector<HashItem> hashtable; // index to chain
        std::vector<ChainItem> chaintable;
        uint16_t next_override_item{0};
        const uint8_t *data_begin;
        const uint8_t *data_end;
        const uint8_t *ip;
        const uint8_t *idxp;
        const uint8_t *emitp;
        Put_function put;

        inline bool no_collision(const uint8_t *idx) const
        {
            idx = hashtable[hash_of(idx)].idx; // idx to hashed-idx =)
            return idx == nullptr || (emitp - idx) > MAX_OFFSET;
        }

        inline auto far_collision(const uint8_t *idx) const
        {
            auto hash = hash_of(idx);
            auto index = hashtable[hash].idx;
            return (index == nullptr) ? MAX_OFFSET : (idx - index);
        }

        auto hash_of_4(const uint8_t *idx)
        {
            alignas(16) uint16_t hashes[4];
            hash_of_sse42(idx, hashes);
            return std::array{&hashtable[hashes[0]],&hashtable[hashes[1]], &hashtable[hashes[2]], &hashtable[hashes[3]]};
        }

        auto hash_of_4_with_filter(const uint8_t *idx)
        {
            alignas(16) uint16_t hashes[4];
            hash_of_sse42(idx, hashes);
            return std::array{
                &hashtable[hashes[0]],
                hashes[1] == hashes[0] ? nullptr : &hashtable[hashes[1]],
                hashes[2] == hashes[1] || hashes[2] == hashes[0] ? nullptr :  &hashtable[hashes[2]],
                hashes[3] == hashes[2] || hashes[3] == hashes[1] ||  hashes[3] == hashes[0] ? nullptr :  &hashtable[hashes[3]]
            };

        }


        inline void register_chain_items( const uint8_t *idx)
        {
            for (auto h : hash_of_4(idx))
            {
                // в хэш-таблице ссылка на следующий элемент в цепочке. там находится размется между текущем смещением и текущим.
                auto &hash_item = *h;
                if (hash_item.idx == nullptr)
                {
                    hash_item.idx = idx++;
                    hash_item.next_item = CHAIN_BREAK;
                    continue;
                }
                if (hash_item.idx + 8 >= idx)
                {
                    idx++;
                    continue;
                }
                uint16_t id = next_override_item;
                if (id + 1 == CHAIN_BREAK)
                    next_override_item = 0; // round
                else
                    next_override_item = id + 1;

                auto &chain_item = chaintable[id];
                chain_item.next_item = hash_item.next_item;
                chain_item.offset_relative = std::min<ptrdiff_t>(0xFFFF, idx - hash_item.idx);
                hash_item.idx = idx++;
                hash_item.next_item = id;
            }
        }



    public:
        TokenSearcher(const uint8_t *begin, const uint8_t *end, Put_function put) : put(put), idxp(begin), ip(begin), emitp(begin), data_begin(begin), data_end(end), hashtable(HASH_SIZE, HashItem{}), chaintable(CHAIN_SIZE, ChainItem{}) {}

        void emit(const Best &best)
        {

            assert(best.ip2 >= emitp);
            assert(best.ofs < best.ip2);
            if (best.len < ENCODE_MIN)
            {
                emit(); // emit as literal
                return;
            }
            put(best.test_ofs(), best.len, emitp, best.ip2 - emitp);
            emitp = ip = best.ip2 + best.len;
        }

        void emit()
        {
            assert(ip >= emitp);
            if (ip > emitp)
            {
                put(0, 0, emitp, ip - emitp);
                emitp = ip;
            }
        }


        void compress()
        {
            while (ip + 7 <= data_end)
            {
                while (idxp+4 < ip)
                {
                    register_chain_items(idxp);
                    idxp+=4;
                }

                Best match{};
                for (auto h : hash_of_4_with_filter(ip))
                {
                    if ((h != nullptr) && (h->idx != nullptr))
                        search_best(*h, match);
                    ip += 1;
                }
                if (match) {
                    emit(match);
                }
            }
            while (ip + 4 <= data_end) {
                Best match{};
                search_best(hashtable[ hash_of(ip) ], match);
                if (match)
                    emit(match);
                else
                    ip += 1;
            }
            ip = data_end;
            emit();
        }

        void search_best(const HashItem& hash_item, Best &best, int nbAttemptsMax = MAX_ATTEMPTS) const
        {
            const uint8_t *idx{hash_item.idx};
            if (idx == nullptr)
                return;
            int next_item = hash_item.next_item;
            int misses = 0;
            for (int nbAttempts = 0; nbAttempts < nbAttemptsMax; nbAttempts++)
            {
                if (nbAttempts > 0)
                {
                    if (next_item == CHAIN_BREAK)
                        break;
                    auto &chain_item = chaintable[next_item];
                    idx -= chain_item.offset_relative;
                    if (idx < data_begin || (emitp - idx > MAX_OFFSET))
                        break;
                    next_item = chain_item.next_item;
                }
                if (idx >= ip)
                    continue;

                // быстрый отсев: кандидат не длиннее лучшего, если не совпал байт, на котором лучший кончается
                if (best)
                {
                    auto k = best.ip2 + best.len - ip;
                    if (k > 0 && ip + k < data_end && idx[k] != ip[k])
                    {
                        if (++misses >= MISS_STREAK)
                            break;
                        continue;
                    }
                }

                auto match_len = match_len_simd(ip, data_end, idx, data_end);
                if (match_len < ENCODE_MIN)
                {
                    // кольцо chaintable переиспользуется: чужой хэш значит, что дальше цепочка протухла
                    if (hash_of(idx) != hash_of(ip))
                        break;
                    if (++misses >= MISS_STREAK)
                        break;
                    continue;
                }

                // idx не трогаем: следующий шаг по цепочке отсчитывается от него
                int back_match_len = match_len_simd_backward(data_begin, idx - 1, emitp, ip - 1);
                Best after{idx - back_match_len, ip - back_match_len, match_len + back_match_len};
                assert(after.ip2 >= emitp);
                assert(after.ofs >= data_begin);
                after.optimize(emitp);

                if (after.gain > best.gain)
                {
                    best = after;
                    misses = 0;
                }
                else if (++misses >= MISS_STREAK)
                    break;
            }
        }
    };

    inline void compress(const uint8_t *begin, const uint8_t *end, Put_function put)
    {
        auto current = begin;
        int plain_len = 0;
        auto ts = TokenSearcher(begin, end, put);
        ts.compress();
    }

    /* expand operators */
    using Put_bytes = std::function<void(uint32_t fetch, int n)>;
    using Move_bytes = std::function<void(int offset, int len)>;
    using Copy_bytes = std::function<void(const uint8_t *src, int len)>;
    using Fill_pattern = std::function<void(int pattern_size, uint32_t pattern, int count)>;

    inline void expand(const uint8_t *begin, const uint8_t *end, Put_bytes put, Move_bytes move, Copy_bytes copy, Fill_pattern fill_pattern)
    {

        auto in = begin;
        uint32_t value32;
        int bytes_available = 0;

        auto fetch = [&]()
        {
            in -= bytes_available;
            if (end - in >= 4)
            {
                bytes_available = 4;
                value32 = load_u32(in);
                in += 4;
                return;
            }
            value32 = 0;
            bytes_available = end - in;
            switch (bytes_available)
            {
            case 1:
                value32 = in[0];
                break;
            case 2:
                value32 = in[0] | (in[1] << 8);
                break;
            case 3:
                value32 = in[0] | (in[1] << 8) | (in[2] << 16);
                break;
            }
            in += bytes_available;
        };
        auto get8_safe = [&]() -> uint8_t
        {
            if (bytes_available == 0)
            {
                fetch();
            }
            uint8_t r = value32 & 0xFF;
            value32 >>= 8;
            bytes_available--;
            return r;
        };
        auto skip = [&](int count)
        {
            bytes_available -= count;
            value32 >>= count * 8;
        };

        for (;;)
        {
            fetch();
            if (value32 == 0)
            {
                return;
            }
            auto token = get8_safe();

            if (token & 0x80)
            {
                // short
                // 1_xx_LL_MMM xxxx_xxxx
                int match_len = token & 0x7;
                int literals_len = (token >> 3) & 0x3;
                int ofs = ((token << 3) & 0x300);
                uint32_t litval{0};
                if (literals_len > 0)
                {
                    litval = value32 & ((1 << (literals_len * 8)) - 1);
                    skip(literals_len);
                }
                ofs |= get8_safe();

                if (match_len == 7)
                {
                    for (;;)
                    {
                        const auto chunk = get8_safe();
                        match_len += chunk;
                        if (chunk != 255)
                            break;
                    }
                }
                if (ofs == 0)
                {
                    // offset == 0:
                    //    LL == 0:  literal match_len count
                    //    LL == 1:  rle x * match_len
                    //    LL == 2:  rle xy * match_len
                    //    LL == 3:  rle xyz * match_len
                    switch (literals_len)
                    {
                    case 0:
                        if (bytes_available >= match_len)
                        {
                            put(value32, match_len);
                            skip(match_len);
                        }
                        else
                        {
                            in -= bytes_available;
                            bytes_available = 0;
                            copy(in, match_len);
                            in += match_len;
                        }
                        break;
                    case 1:
                    case 2:
                    case 3:
                        fill_pattern(literals_len, litval, match_len);
                        break;
                    }
                }
                else
                {
                    if (literals_len > 0)
                        put(litval, literals_len);
                    match_len += ENCODE_MIN;
                    move(ofs, match_len);
                }
            }
            else
            {
                // 0_x_LLL_MMM xxxx_xxxx xxxx_xxxx
                int literals_len = (token >> 3) & 0x7;
                int match_len = token & 0x7;
                if (literals_len == 7)
                {
                    for (;;)
                    {
                        const auto chunk = get8_safe();
                        literals_len += chunk;
                        if (chunk != 255)
                            break;
                    }
                }
                if (bytes_available >= literals_len)
                {
                    put(value32, literals_len);
                    skip(literals_len);
                }
                else
                {
                    in -= bytes_available;
                    bytes_available = 0;
                    copy(in, literals_len);
                    in += literals_len;
                }
                int ofs = get8_safe();
                ofs |= get8_safe() << 8;
                ofs |= (token << (-6 + 16)) & 0x10000;
                if (match_len == 7)
                {
                    for (;;)
                    {
                        const auto chunk = get8_safe();
                        match_len += chunk;
                        if (chunk != 255)
                            break;
                    }
                }
                match_len += ENCODE_MIN;
                move(ofs, match_len);
            }
        }
    }


    /* быстрый декодер в буфер */

    // Запас за концом выходного буфера: wildcopy пишет кусками по 16 байт и может уйти за конец токена.
    constexpr int DECODE_SLACK = 32;

    enum class DecodeStatus
    {
        Done,       // вход кончился на границе токена
        NeedOutput, // следующему токену нужно need байт выхода (включая DECODE_SLACK)
        Error       // поток битый, причина в error
    };

    struct DecodeResult
    {
        const uint8_t *in;  // начало первого не декодированного токена
        uint8_t *op;        // конец записанного
        DecodeStatus status;
        int64_t need{0};
        const char *error{nullptr};
    };

    // Декодирует токены [in, in_end) в op. Пишет только ниже op_end; токен берётся, лишь если
    // ему хватает места вместе с DECODE_SLACK, иначе возвращает NeedOutput на его начале.
    // window — самый ранний байт, на который может ссылаться матч.
    inline DecodeResult decode(const uint8_t *in, const uint8_t *in_end, uint8_t *op, uint8_t *op_end, const uint8_t *window)
    {
        auto fail = [&](const uint8_t *at, const char *why) { return DecodeResult{at, op, DecodeStatus::Error, 0, why}; };
        // расширение длины: v += байты, пока байт == 255
        auto read_ext = [&](const uint8_t *&p, int64_t &v) -> bool
        {
            for (;;)
            {
                if (p >= in_end)
                    return false;
                const uint8_t c = *p++;
                v += c;
                if (c != 255)
                    return v < (int64_t(1) << 31);
            }
        };
        auto copy_literals = [&](const uint8_t *src, int64_t n)
        {
            if (in_end - src >= n + 16)
            {
                uint8_t *d = op, *e = op + n;
                do { std::memcpy(d, src, 16); d += 16; src += 16; } while (d < e);
            }
            else
                std::memcpy(op, src, n);
            op += n;
        };
        auto copy_match = [&](int64_t ofs, int64_t len)
        {
            const uint8_t *src = op - ofs;
            uint8_t *d = op, *e = op + len;
            if (ofs >= 16)
                do { std::memcpy(d, src, 16); d += 16; src += 16; } while (d < e);
            else if (ofs >= 8)
                do { std::memcpy(d, src, 8); d += 8; src += 8; } while (d < e);
            else
                for (; d < e; ++d) *d = *(d - ofs);
            op = e;
        };

        for (;;)
        {
            const uint8_t *tok = in;
            if (in >= in_end)
                return {in, op, DecodeStatus::Done};
            {
                // совместимость с expand: нулевые байты на границе токена = конец потока
                uint32_t v = 0;
                if (in_end - in >= 4)
                    v = load_u32(in);
                else
                    std::memcpy(&v, in, in_end - in);
                if (v == 0)
                    return {in_end, op, DecodeStatus::Done};
            }
            const uint8_t t = *in++;
            const uint8_t *lit;
            int64_t lits, len, ofs;
            if (t & 0x80)
            {
                // 1_xx_LL_MMM lit[LL] ofs_lo [ext]
                lits = (t >> 3) & 0x3;
                len = t & 0x7;
                if (in_end - in < lits + 1)
                    return fail(tok, "truncated token");
                lit = in;
                in += lits;
                ofs = ((t << 3) & 0x300) | *in++;
                if (len == 7 && !read_ext(in, len))
                    return fail(tok, "truncated length");
                if (ofs == 0)
                {
                    if (lits == 0)
                    {
                        // только литералы: их число в MMM
                        lits = len;
                        if (in_end - in < lits)
                            return fail(tok, "truncated literals");
                        if (op_end - op < lits + DECODE_SLACK)
                            return {tok, op, DecodeStatus::NeedOutput, lits + DECODE_SLACK};
                        copy_literals(in, lits);
                        in += lits;
                    }
                    else
                    {
                        // паттерн из LL байт, повторённый MMM раз
                        const int64_t n = lits * len;
                        if (op_end - op < n + DECODE_SLACK)
                            return {tok, op, DecodeStatus::NeedOutput, n + DECODE_SLACK};
                        for (int64_t i = 0; i < n; ++i)
                            op[i] = lit[i % lits];
                        op += n;
                    }
                    continue;
                }
            }
            else
            {
                // 0_x_LLL_MMM [ext] lit[n] ofs_lo ofs_hi [ext]
                lits = (t >> 3) & 0x7;
                len = t & 0x7;
                if (lits == 7 && !read_ext(in, lits))
                    return fail(tok, "truncated literal length");
                if (in_end - in < lits + 2)
                    return fail(tok, "truncated token");
                lit = in;
                in += lits;
                ofs = in[0] | (in[1] << 8) | ((t << 10) & 0x10000);
                in += 2;
                if (len == 7 && !read_ext(in, len))
                    return fail(tok, "truncated length");
                if (ofs == 0)
                    return fail(tok, "zero offset");
            }
            len += ENCODE_MIN;
            if (op_end - op < lits + len + DECODE_SLACK)
                return {tok, op, DecodeStatus::NeedOutput, lits + len + DECODE_SLACK};
            if (ofs > (op + lits) - window)
                return fail(tok, "offset before window");
            copy_literals(lit, lits);
            copy_match(ofs, len);
        }
    }

}

#endif // LZ7_H
