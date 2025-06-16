#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define _USE_SIMD
#define MAX_OFFSET ((1 << 17) - 1)
#define NO_MATCH_OFS (MAX_OFFSET)
#define ENCODE_MIN (4)
#define MAX_ATTEMPTS (16)
#define HASH_LOG2 (16)
#define HASH_SIZE (1 << HASH_LOG2)
#define CHAIN_LOG2 (16)
#define CHAIN_SIZE (1 << CHAIN_LOG2)
#define CHAIN_BREAK (CHAIN_SIZE - 1)
#define LOOK_AHEAD (2)
#define RLE_INDEX_TRIGGER 10
#define RLE_INDEX_DISTANCE 0x1000
#define REP2_INDEX_DISTANCE 0x1000
#define RLE_ACCEPTABLE_GAIN 0x1000
#define BEST_ACCEPTABLE_GAIN 8
#define LOOK_AHEAD_ACCEPTABLE_GAIN 0x1000
// #define _USE_FAST_SKIP
#define _USE_JUMP_OVER_MATCH 0
// #define _ENABLE_RLE
#define CHECK_IP_END (sizeof(uint32_t))
#if CHAIN_LOG2 > 16
#error "CHAIN_LOG2 > chain is uint16_t only"
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
#include <immintrin.h>

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

    inline uint32_t match_of(const uint8_t *it)
    {
#if ENCODE_MIN == 3
        return (reinterpret_cast<const uint32_t *>(it)[0] & 0xffffff);
#else
        return (reinterpret_cast<const uint32_t *>(it)[0]);
#endif
    }

    inline uint16_t hash_of(const uint8_t *it)
    {
        return static_cast<uint16_t>(match_of(it) * 2654435761u >> (32 - HASH_LOG2));
    }

    // SSE4.2 реализация hash_of для 4 последовательных 32-битных чисел
    inline void hash_of_sse42(const uint8_t *it, uint16_t *hashes)
    {
        // Загрузить 4 последовательных 32-битных числа
        __m128i data = _mm_set_epi32(
            reinterpret_cast<const uint32_t *>(it + 3)[0],
            reinterpret_cast<const uint32_t *>(it + 2)[0],
            reinterpret_cast<const uint32_t *>(it + 1)[0],
            reinterpret_cast<const uint32_t *>(it + 0)[0]);

        // Константа для хэширования (множитель)
        __m128i multiplier = _mm_set1_epi32(2654435761u);

        // Умножить числа на константу (оставить младшие 32 бита)
        __m128i hash_values = _mm_mullo_epi32(data, multiplier);

        // Сдвиг вправо для получения HASH_LOG2 бит
        __m128i shifted = _mm_srli_epi32(hash_values, 32 - HASH_LOG2);

        // Преобразовать в 16-битные значения (младшие 16 бит результата)
        __m128i result = _mm_packus_epi32(shifted, shifted);

        // Сохранить результат в массив hash
        _mm_storel_epi64(reinterpret_cast<__m128i *>(hashes), result);
    }

    inline int match_len_simd(const uint8_t *a, const uint8_t *a_end, const uint8_t *b, const uint8_t *b_end)
    {
        int len = 0;
#ifdef _USE_SIMD
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
#ifdef _USE_SIMD
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

        int test_ofs() const
        {
            return std::distance(ofs, ip2);
        }
        auto test_short() const
        {
            return test_ofs() < (1 << 10);
        }
        auto is_short(const uint8_t *emitp) const
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

        static int literal_cost(unsigned long nlit)
        {
            return (nlit + 255 - 7) / 255;
        }

        static int match_cost(unsigned long len)
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
        const uint8_t *force_index_rle{nullptr};

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

        void register_chain_item(const uint8_t *idx, int distance = 0)
        {
            if (idx + CHECK_IP_END >= data_end)
                return;
            auto hash = hash_of(idx);
            // в хэш-таблице ссылка на следующий элемент в цепочке. там находится размется между текущем смещением и текущим.
            auto &hash_item = hashtable[hash];
            if (hash_item.idx == nullptr)
            {
                hash_item.idx = idx;
                hash_item.next_item = CHAIN_BREAK;
                return;
            }
            if (hash_item.idx + distance >= idx)
            {
                return;
            }
            uint16_t id = next_override_item;
            if (id + 1 == CHAIN_BREAK)
                next_override_item = 0; // round
            else
                next_override_item = id + 1;

            auto &chain_item = chaintable[id];
            chain_item.next_item = hash_item.next_item;
            chain_item.offset_relative = std::min<ptrdiff_t>(0xFFFF, idx - hash_item.idx);
            hash_item.idx = idx;
            hash_item.next_item = id;
        }
        inline void register_chain_item(const uint8_t *idx, const uint16_t* hashes, int distance = 0, int nb=4)
        {
            for (int i = 0; i < nb; i++, idx++, hashes++)
            {
                // в хэш-таблице ссылка на следующий элемент в цепочке. там находится размется между текущем смещением и текущим.
                auto &hash_item = hashtable[*hashes];
                if (hash_item.idx == nullptr)
                {
                    hash_item.idx = idx;
                    hash_item.next_item = CHAIN_BREAK;
                    continue;
                }
                if (hash_item.idx + distance >= idx)
                {
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
                hash_item.idx = idx;
                hash_item.next_item = id;
            }
        }
        Best index_rle_search()
        {
            Best rle{};
            index();
#ifdef _ENABLE_RLE
            if (is_rle(ip))
            {
                if (no_collision(ip))
                    force_index_rle = ip;
                rle.len = match_len_simd(ip, data_end, idxp + 1, data_end);
                idxp = ip + 1 + rle.len;
                rle.ofs = ip;
                rle.ip2 = ip + 1;
                rle.optimize(emitp);
            }
            else if (is_rep2(ip))
            {
                if (no_collision(ip))
                    force_index_rle = ip;
                rle.len = match_len_simd(ip, data_end, idxp + 2, data_end);
                idxp = ip + 2 + rle.len;
                rle.ofs = ip;
                rle.ip2 = ip + 2;
                rle.optimize(emitp);
            }
            if (rle.gain >= RLE_INDEX_TRIGGER)
            {
                force_index_rle = ip;
                return rle;
            }
#endif
            return search_best( hashtable[hash_of(ip)], rle);
        }

        void index()
        {
            if (force_index_rle != nullptr)
            {
                register_chain_item(force_index_rle, RLE_INDEX_DISTANCE);
                force_index_rle = nullptr;
            }
#if _USE_JUMP_OVER_MATCH > 0
            while (idxp + _USE_JUMP_OVER_MATCH < emitp)
            {
                register_chain_item(idxp);
                idxp += _USE_JUMP_OVER_MATCH;
            }
#endif
            while (idxp < ip)
            {
                register_chain_item(idxp);
                idxp++;
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
            alignas(16) uint16_t hashes[4];
            while (ip + 7 <= data_end)
            {
                while (idxp+4 < ip)
                {
                    hash_of_sse42(idxp, hashes);
                    register_chain_item(idxp, hashes,2);
                    idxp+=4;
                }

                hash_of_sse42(ip, hashes);

                auto best = search_best(hashtable[ hashes[0] ], {});
                ip += 1;
                if (hashes[0] != hashes[1] )
                    best = search_best(hashtable[ hashes[1] ],best);
                ip += 1;
                if (hashes[0] != hashes[2] && hashes[1] != hashes[2] )
                    best = search_best(hashtable[ hashes[2] ],best);
                ip += 1;
                if (hashes[0] != hashes[3]  && hashes[1] != hashes[3]  && hashes[2] != hashes[3])
                    best = search_best(hashtable[ hashes[3] ], best);
                ip += 1;
                if (best.len >= ENCODE_MIN)
                    emit(best);
            }
            while (ip + 4 <= data_end) {
                auto best = search_best(hashtable[ hash_of(ip) ], {});
                if (best.len >= ENCODE_MIN)
                    emit(best);
                else
                    ip += 1;
            }
            emit();
        }

//         void compress()
//         {
//             compress_sse42();
//             if (ip > data_end)
//             {
//                 return;
//             }
//             for (;;)
//             {
//                 if (ip + CHECK_IP_END > data_end)
//                 {
//                     ip = data_end;
//                     emit();
//                     break;
//                 }
//                 Best match = index_rle_search();
//                 if (match.len < ENCODE_MIN)
//                 {
//                     ip++;
//                     continue;
//                 }
// #if LOOK_AHEAD > 0
//                 if (match.gain < LOOK_AHEAD_ACCEPTABLE_GAIN)
//                 {
//                     auto ip_last = emitp + LOOK_AHEAD;
//                     if (ip_last + CHECK_IP_END <= data_end && ip + 1 <= ip_last)
//                     {
//                         uint32_t p_m, pp_m;
//                         p_m = pp_m = match_of(ip);
//                         ++ip;
//                         for (; ip <= ip_last; ip++)
//                         {
//                             auto m = match_of(ip);
//                             if (m == p_m || m == pp_m)
//                             {
//                                 break;
//                             }
//                             pp_m = p_m;
//                             p_m = m;
//                             auto next_match = search_best(hashtable[hash_of(ip)], match);
//                             if (next_match.gain > match.gain)
//                             {
//                                 match = next_match;
//                             }
//                             else if (match.gain >= LOOK_AHEAD_ACCEPTABLE_GAIN)
//                             {
//                                 break;
//                             }
//                         }
//                     }
//                 }
// #endif
//                 emit(match);
//             }
//         }

        Best search_best(HashItem hash_item, Best best = {}, int nbAttemptsMax = MAX_ATTEMPTS) const
        {
            // bool is_literal_packet = ip - emitp > 3;
            // if ( is_literal_packet && best.len >= ENCODE_MIN)
            //     return best;
            // if (best.gain > BEST_ACCEPTABLE_GAIN)
            //     return best;
            const uint8_t *idx{hash_item.idx};
            if (idx == nullptr)
                return best;
            int next_item = hash_item.next_item;
            for (int nbAttempts = 0; nbAttempts < nbAttemptsMax; nbAttempts++)
            {
                if (nbAttempts > 0)
                {
                    if (next_item == CHAIN_BREAK)
                        break;
                    auto &chain_item = chaintable[next_item];
                    idx -= chain_item.offset_relative;
                    if (idx < data_begin || (emitp-idx > MAX_OFFSET))
                        break;
                    next_item = chain_item.next_item;
                }
                // pre-hashing happens...
                // fast skip
                if (idx >= ip)
                    continue;

#ifdef _USE_mismatch
                auto [it_mismatch, ip_mismatch] = std::mismatch(it, data_end, ip, data_end);
                int match_len = std::distance(it, it_mismatch);
#else
                auto match_len = match_len_simd(ip, data_end, idx, data_end);
#endif

                if (match_len < ENCODE_MIN) {
                    if (hash_of(idx) != hash_of(ip))
                        break;
                    continue;
                }

                int back_match_len = match_len_simd_backward(data_begin, idx - 1, emitp, ip - 1);
                auto ip2 = ip - back_match_len;
                idx -= back_match_len;
                match_len += back_match_len;

                assert(ip2 >= emitp);
                assert(idx >= data_begin);
                // assert(it + match <= ip2);

                Best after{idx, ip2, match_len};
                after.optimize(emitp);

                if (after.gain > best.gain)
                {
                    best = after;
                    // if (best.is_short(emitp))
                    //     break;

                    // std::cerr << " gain search step: " << (best.gain - after.gain) << std::endl;
                }
            }
            return best;
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

    void expand(const uint8_t *begin, const uint8_t *end, Put_bytes put, Move_bytes move, Copy_bytes copy, Fill_pattern fill_pattern)
    {
        std::vector<uint8_t> out;
        out.reserve(1 << 20);
        auto in = begin;
        uint32_t value32;
        int bytes_available = 0;

        auto fetch = [&]()
        {
            in -= bytes_available;
            if (end - in >= 4)
            {
                bytes_available = 4;
                value32 = reinterpret_cast<const uint32_t *>(in)[0];
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

}

#endif // LZ7_H
