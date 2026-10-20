// https://godbolt.org/z/1fd69Ez7W
#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define MAX_OFFSET ((1 << 17) - 1)
#define NO_MATCH_OFS (MAX_OFFSET)
#define ENCODE_MIN (4)
// Жёсткий потолок кандидатов на один обход цепочки; обычно обход кончается раньше,
// по серии промахов уровня сжатия (lz7::LEVELS).
#ifndef MAX_ATTEMPTS
#define MAX_ATTEMPTS (256)
#endif
#define HASH_LOG2 (16)
#define HASH_SIZE (1 << HASH_LOG2)
#define CHAIN_LOG2 (17)
#define CHAIN_SIZE (1 << CHAIN_LOG2)
#define CHAIN_MASK (CHAIN_SIZE - 1)
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
#if CHAIN_SIZE <= MAX_OFFSET
#error "chain ring must cover the whole window"
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

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "lz7: little-endian only (match_len uses ctz/clz on 64-bit loads)"
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
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

    inline uint16_t load_u16(const uint8_t *p)
    {
        uint16_t v;
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

    inline uint64_t load_u64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    // Хэш для 4 соседних позиций: hashes[i] = hash_of(it + i).
    inline void hash_of_x4(const uint8_t *it, uint16_t *hashes)
    {
        for (int i = 0; i < 4; ++i)
            hashes[i] = hash_of(it + i);
    }

    // ctz/clz для ненулевого x: builtin GCC/Clang, у MSVC — _BitScan*, на 32-битных целях по половинам.
    inline int ctz64(uint64_t x)
    {
#if defined(_MSC_VER) && !defined(__clang__)
        unsigned long i;
#if defined(_M_X64) || defined(_M_ARM64)
        _BitScanForward64(&i, x);
        return int(i);
#else
        if (_BitScanForward(&i, static_cast<unsigned long>(x)))
            return int(i);
        _BitScanForward(&i, static_cast<unsigned long>(x >> 32));
        return int(i) + 32;
#endif
#else
        return __builtin_ctzll(x);
#endif
    }

    inline int clz64(uint64_t x)
    {
#if defined(_MSC_VER) && !defined(__clang__)
        unsigned long i;
#if defined(_M_X64) || defined(_M_ARM64)
        _BitScanReverse64(&i, x);
        return 63 - int(i);
#else
        if (_BitScanReverse(&i, static_cast<unsigned long>(x >> 32)))
            return 31 - int(i);
        _BitScanReverse(&i, static_cast<unsigned long>(x));
        return 63 - int(i);
#endif
#else
        return __builtin_clzll(x);
#endif
    }

    // Длина совпадения a[0..] и b[0..]: xor 8-байтных слов, первая разница через ctz.
    inline int match_len(const uint8_t *a, const uint8_t *a_end, const uint8_t *b, const uint8_t *b_end)
    {
        int len = 0;
        while (a_end - a >= 8 && b_end - b >= 8)
        {
            const uint64_t x = load_u64(a) ^ load_u64(b);
            if (x)
                return len + (ctz64(x) >> 3);
            len += 8;
            a += 8;
            b += 8;
        }
        while (a < a_end && b < b_end && *a == *b)
        {
            ++a;
            ++b;
            ++len;
        }
        return len;
    }

    // Число совпадающих байт перед a и b (a[-1] == b[-1], ...), не заходя за a_begin/b_begin.
    // Старший байт 8-байтного слова — ближайший к a, поэтому clz.
    inline int match_len_backward(const uint8_t *a_begin, const uint8_t *a, const uint8_t *b_begin, const uint8_t *b)
    {
        int len = 0;
        while (a - a_begin >= 8 && b - b_begin >= 8)
        {
            const uint64_t x = load_u64(a - 8) ^ load_u64(b - 8);
            if (x)
                return len + (clz64(x) >> 3);
            len += 8;
            a -= 8;
            b -= 8;
        }
        while (a > a_begin && b > b_begin && a[-1] == b[-1])
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

    // Уровень сжатия: сколько соседних позиций проверять за шаг (1..4) и после скольких
    // кандидатов подряд без улучшения бросать обход цепочки. Подобрано замерами на
    // Calgary/sqlite3.c/libstdc++/enwik8: 3 позиции выгоднее 4 на любом бюджете промахов,
    // кроме максимального.
    // lazy_searches != 0: ленивый разбор по образцу LZ4HC (LZ4HC_compress_hashChain) с этим
    // числом кандидатов на поиск; lookahead и miss_streak тогда не используются.
    struct Level
    {
        int lookahead;
        int miss_streak;
        int lazy_searches = 0;
    };
    constexpr Level LEVELS[] = {{3, 4}, {3, 8}, {3, 16}, {4, 16}, {0, 0, 64}};
    constexpr int MIN_LEVEL = 1;
    constexpr int MAX_LEVEL = 5;
    constexpr int DEFAULT_LEVEL = 2;

    class TokenSearcher
    {
        // Цепочки по позиции, как в LZ4HC: звено позиции pos лежит в chain[pos & CHAIN_MASK]
        // и хранит расстояние до предыдущей позиции с тем же хэшем (0 = конец цепочки).
        // Кольцо покрывает всё окно, поэтому звено позиции внутри окна не перезаписано.
        // head[h] = (позиция + 1) mod 2^32 последней вставки с хэшем h, 0 = пусто; расстояния
        // считаются по модулю 2^32, поэтому вход больше 4 ГБ допустим: кандидат из устаревшей
        // головы всё равно проверяется сравнением байт.
        std::vector<uint32_t> head;
        std::vector<uint32_t> chain;
        const uint8_t *data_begin;
        const uint8_t *data_end;
        const uint8_t *ip;
        const uint8_t *idxp;
        const uint8_t *emitp;
        Put_function put;
        Level level;

        inline uint32_t pos_of(const uint8_t *p) const
        {
            return uint32_t(p - data_begin);
        }

        // кандидат из головы цепочки для позиции p или nullptr
        inline const uint8_t *head_candidate(uint32_t hp, const uint8_t *p) const
        {
            if (hp == 0)
                return nullptr;
            const size_t dist = uint32_t(pos_of(p) - (hp - 1));
            if (dist == 0 || dist > MAX_OFFSET || dist > size_t(p - data_begin))
                return nullptr;
            return p - dist;
        }

        inline size_t chain_delta(const uint8_t *p) const
        {
            return chain[size_t(p - data_begin) & CHAIN_MASK];
        }

        inline bool no_collision(const uint8_t *idx) const
        {
            const uint8_t *c = head_candidate(head[hash_of(idx)], idx);
            return c == nullptr || (emitp - c) > MAX_OFFSET;
        }

        inline auto far_collision(const uint8_t *idx) const
        {
            const uint8_t *c = head_candidate(head[hash_of(idx)], idx);
            return c == nullptr ? MAX_OFFSET : (idx - c);
        }

        // головы цепочек для 4 соседних позиций; повтор хэша среди них -> 0 (уже обойдён)
        auto heads_of_4_with_filter(const uint8_t *idx) const
        {
            alignas(16) uint16_t h[4];
            hash_of_x4(idx, h);
            return std::array<uint32_t, 4>{
                head[h[0]],
                h[1] == h[0] ? 0u : head[h[1]],
                h[2] == h[1] || h[2] == h[0] ? 0u : head[h[2]],
                h[3] == h[2] || h[3] == h[1] || h[3] == h[0] ? 0u : head[h[3]]};
        }

        // вставить 4 позиции подряд
        inline void register_chain_items(const uint8_t *idx)
        {
            alignas(16) uint16_t h[4];
            hash_of_x4(idx, h);
            for (int k = 0; k < 4; ++k)
            {
                const uint8_t *const p = idx + k;
                const uint8_t *const prev = head_candidate(head[h[k]], p);
                const size_t d = prev ? size_t(p - prev) : 0;
                chain[size_t(p - data_begin) & CHAIN_MASK] = uint32_t(d);
                head[h[k]] = pos_of(p) + 1;
            }
        }

    public:
        TokenSearcher(const uint8_t *begin, const uint8_t *end, Put_function put, Level level) : put(put), level(level), idxp(begin), ip(begin), emitp(begin), data_begin(begin), data_end(end), head(HASH_SIZE, 0), chain(CHAIN_SIZE, 0) {}

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
                auto hs = heads_of_4_with_filter(ip);
                for (int k = 0; k < level.lookahead; ++k)
                {
                    if (hs[k])
                        search_best(hs[k], match);
                    ip += 1;
                }
                if (match) {
                    emit(match);
                }
            }
            while (ip + 4 <= data_end) {
                Best match{};
                search_best(head[hash_of(ip)], match);
                if (match)
                    emit(match);
                else
                    ip += 1;
            }
            ip = data_end;
            emit();
        }

        // оценить одного кандидата; false = бросить обход
        bool try_candidate(const uint8_t *idx, Best &best, int &misses) const
        {
            // быстрый отсев: кандидат не длиннее лучшего, если не совпал байт, на котором лучший кончается
            if (best)
            {
                auto k = best.ip2 + best.len - ip;
                if (k > 0 && ip + k < data_end && idx[k] != ip[k])
                    return ++misses < level.miss_streak;
            }
            auto match_len = lz7::match_len(ip, data_end, idx, data_end);
            if (match_len < ENCODE_MIN) // коллизия хэша
                return ++misses < level.miss_streak;
            int back_match_len = match_len_backward(data_begin, idx, emitp, ip);
            Best after{idx - back_match_len, ip - back_match_len, match_len + back_match_len};
            assert(after.ip2 >= emitp);
            assert(after.ofs >= data_begin);
            after.optimize(emitp);
            if (after.gain > best.gain)
            {
                best = after;
                misses = 0;
                return true;
            }
            return ++misses < level.miss_streak;
        }

        void search_best(uint32_t hp, Best &best, int nbAttemptsMax = MAX_ATTEMPTS) const
        {
            const uint8_t *idx = head_candidate(hp, ip);
            if (idx == nullptr)
                return;
            int misses = 0;
            for (int nbAttempts = 0; nbAttempts < nbAttemptsMax; nbAttempts++)
            {
                if (nbAttempts > 0)
                {
                    const size_t d = chain_delta(idx);
                    if (d == 0 || d > size_t(idx - data_begin))
                        break;
                    idx -= d;
                    if (emitp - idx > MAX_OFFSET)
                        break;
                }
                if (idx >= ip)
                    continue;
                if (!try_candidate(idx, best, misses))
                    break;
            }
        }

        // ---- ленивый разбор по образцу LZ4HC_compress_hashChain ----
        struct LazyMatch
        {
            int len;     // 0 = нет матча
            size_t off;  // смещение
            int score;   // выгода: len минус заголовок токена
        };

        // байты заголовка токена lz7, которые зависят от выбора матча: токен, смещение и
        // расширение длины матча. Расширение длины литералов не считается: серию литералов
        // всё равно закроет какой-то токен.
        static constexpr int token_cost(size_t lits, int len, size_t off)
        {
            const int me = len - ENCODE_MIN >= 7 ? (len - ENCODE_MIN - 7) / 255 + 1 : 0;
            return (off < 1024 && lits < 4 ? 2 : 3) + me;
        }

        inline void register_one(const uint8_t *p)
        {
            const uint16_t h = hash_of(p);
            const uint8_t *const prev = head_candidate(head[h], p);
            chain[size_t(p - data_begin) & CHAIN_MASK] = prev ? uint32_t(p - prev) : 0;
            head[h] = pos_of(p) + 1;
        }

        inline void insert_upto(const uint8_t *p)
        {
            for (; idxp < p; ++idxp)
                register_one(idxp);
        }

        // Матч для позиции p, который может начинаться раньше p, но не раньше low
        // (LZ4HC_InsertAndGetWiderMatch). Как в HC, годится только матч длиннее min_len, а из
        // годных берётся самый выгодный по цене формата; литералы перед ним считаются от prev_end.
        LazyMatch find_wider(const uint8_t *p, const uint8_t *low, const uint8_t *prev_end, int min_score, int min_len,
                             const uint8_t *&start, int attempts)
        {
            LazyMatch m{0, 0, 0};
            int longest = std::max(ENCODE_MIN - 1, min_len); // порог для быстрого отсева
            start = p;
            insert_upto(p);
            const uint8_t *cand = head_candidate(head[hash_of(p)], p);
            const uint32_t pattern = load_u32(p);
            const ptrdiff_t look_back = p - low;
            while (cand != nullptr && attempts-- > 0)
            {
                // быстрый отсев по двум байтам там, где кончается текущий лучший
                const uint8_t *const qa = low + longest - 1;
                const uint8_t *const qb = cand - look_back + longest - 1;
                const bool quick = qb < data_begin || qa + 2 > data_end || load_u16(qa) == load_u16(qb);
                if (quick && load_u32(cand) == pattern)
                {
                    int back = 0;
                    while (p - back > low && cand - back > data_begin && p[-back - 1] == cand[-back - 1])
                        ++back;
                    const int len = ENCODE_MIN + lz7::match_len(p + ENCODE_MIN, data_end, cand + ENCODE_MIN, data_end) + back;
                    const size_t off = size_t(p - cand);
                    const uint8_t *const st = p - back;
                    const size_t lits = st > prev_end ? size_t(st - prev_end) : 0;
                    const int score = len - token_cost(lits, len, off);
                    if (len > min_len && score > min_score)
                    {
                        min_score = score;
                        longest = std::max(longest, len - 2);
                        m = {len, off, score};
                        start = st;
                    }
                }
                const size_t d = chain_delta(cand);
                if (d == 0 || d > size_t(cand - data_begin))
                    break;
                cand -= d;
                if (size_t(p - cand) > MAX_OFFSET)
                    break;
            }
            return m;
        }

        void compress_lazy(int nb)
        {
            // OPTIMAL_ML: длина, до которой матч кодируется без байта расширения (поле MMM = 0..6)
            constexpr int OPTIMAL_ML = 6 + ENCODE_MIN;
            constexpr int MFLIMIT = 12;
            const uint8_t *anchor = data_begin;
            auto encode = [&](const uint8_t *&q, const LazyMatch &m)
            {
                if (m.len < ENCODE_MIN) // обрезан до негодного: остаётся литералами
                {
                    q += m.len;
                    return;
                }
                assert(m.off > 0 && m.off <= MAX_OFFSET);
                put(int(m.off), m.len, anchor, int(q - anchor));
                q += m.len;
                anchor = q;
            };
            if (data_end - data_begin > MFLIMIT)
            {
                const uint8_t *const mflimit = data_end - MFLIMIT;
                const uint8_t *ip = data_begin;
                const uint8_t *start0, *start2 = nullptr, *start3 = nullptr;
                LazyMatch m0, m1, m2, m3;
                const LazyMatch none{0, 0, 0};
                while (ip <= mflimit)
                {
                    const uint8_t *s1;
                    m1 = find_wider(ip, ip, anchor, 0, ENCODE_MIN - 1, s1, nb);
                    if (m1.len < ENCODE_MIN)
                    {
                        ++ip;
                        continue;
                    }
                    start0 = ip;
                    m0 = m1;
                search2:
                    if (ip + m1.len <= mflimit)
                        m2 = find_wider(ip + m1.len - 2, ip, ip + m1.len, 0, m1.len, start2, nb);
                    else
                        m2 = none;
                    if (m2.len <= m1.len) // лучше нет: пишем m1
                    {
                        encode(ip, m1);
                        continue;
                    }
                    if (start0 < ip && start2 < ip + m0.len) // m1 пропускали: вернуть исходный
                    {
                        ip = start0;
                        m1 = m0;
                    }
                    if (start2 - ip < 3) // первый слишком короткий: выбросить
                    {
                        ip = start2;
                        m1 = m2;
                        goto search2;
                    }
                search3:
                    if (start2 - ip < OPTIMAL_ML)
                    {
                        int new_ml = std::min(m1.len, OPTIMAL_ML);
                        if (ip + new_ml > start2 + m2.len - ENCODE_MIN)
                            new_ml = int(start2 - ip) + m2.len - ENCODE_MIN;
                        const int correction = new_ml - int(start2 - ip);
                        if (correction > 0)
                        {
                            start2 += correction;
                            m2.len -= correction;
                        }
                    }
                    if (start2 + m2.len <= mflimit)
                        m3 = find_wider(start2 + m2.len - 3, start2, start2 + m2.len, 0, m2.len, start3, nb);
                    else
                        m3 = none;
                    if (m3.len <= m2.len) // третьего нет: пишем m1 и m2
                    {
                        if (start2 < ip + m1.len)
                            m1.len = int(start2 - ip);
                        encode(ip, m1);
                        ip = start2;
                        encode(ip, m2);
                        continue;
                    }
                    if (start3 < ip + m1.len + 3) // m2 не помещается: убрать его
                    {
                        if (start3 >= ip + m1.len) // m1 можно писать сразу, m3 становится m1
                        {
                            if (start2 < ip + m1.len)
                            {
                                const int correction = int(ip + m1.len - start2);
                                start2 += correction;
                                m2.len -= correction;
                                if (m2.len < ENCODE_MIN)
                                {
                                    start2 = start3;
                                    m2 = m3;
                                }
                            }
                            encode(ip, m1);
                            ip = start3;
                            m1 = m3;
                            start0 = start2;
                            m0 = m2;
                            goto search2;
                        }
                        start2 = start3;
                        m2 = m3;
                        goto search3;
                    }
                    // три матча по возрастанию: пишем m1, обрезав его о начало m2
                    if (start2 < ip + m1.len)
                    {
                        if (start2 - ip < OPTIMAL_ML)
                        {
                            if (m1.len > OPTIMAL_ML)
                                m1.len = OPTIMAL_ML;
                            if (ip + m1.len > start2 + m2.len - ENCODE_MIN)
                                m1.len = int(start2 - ip) + m2.len - ENCODE_MIN;
                            const int correction = m1.len - int(start2 - ip);
                            if (correction > 0)
                            {
                                start2 += correction;
                                m2.len -= correction;
                            }
                        }
                        else
                            m1.len = int(start2 - ip);
                    }
                    encode(ip, m1);
                    ip = start2;
                    m1 = m2;
                    start2 = start3;
                    m2 = m3;
                    goto search3;
                }
            }
            if (anchor < data_end)
                put(0, 0, anchor, int(data_end - anchor));
        }

    public:
        void compress_dispatch()
        {
            if (level.lazy_searches)
                compress_lazy(level.lazy_searches);
            else
                compress();
        }
    };

    inline void compress(const uint8_t *begin, const uint8_t *end, Put_function put, int level = DEFAULT_LEVEL)
    {
        level = std::clamp(level, MIN_LEVEL, MAX_LEVEL);
        auto lv = LEVELS[level - 1];
        auto ts = TokenSearcher(begin, end, put, lv);
        ts.compress_dispatch();
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
            // Быстрый цикл по образцу LZ4_decompress_generic: пока до конца входа больше 32 байт, а до
            // конца выхода больше 64, токены разбираются прямо по указателю. Тип токена (короткий/длинный)
            // непредсказуем, поэтому поля обоих вариантов считаются без ветвления и выбираются маской.
            // Литералы до 6 копируются 16 байтами, матч до 10 при смещении >= 8 — двумя по 8, остальное
            // кусками по 32 (по 8 при смещении 8..15, побайтово при < 8). Лишние байты за реальным концом
            // перезапишет следующий токен. Нулевое смещение, смещение за окном, ошибки расширений и
            // близость конца откатываются на начало токена и уходят в общий путь ниже.
            {
                const uint8_t *const in_fast = in_end - 32;
                uint8_t *const op_fast = op_end - 64;
                auto wildcopy32 = [](uint8_t *d, const uint8_t *src, uint8_t *e)
                {
                    do
                    {
                        std::memcpy(d, src, 16);
                        std::memcpy(d + 16, src + 16, 16);
                        d += 32;
                        src += 32;
                    } while (d < e);
                };
                // расширение длины в быстром цикле: указатель за расширением или nullptr, если вышло за limit
                auto fast_ext = [](const uint8_t *p, const uint8_t *limit, size_t &v) -> const uint8_t *
                {
                    for (;;)
                    {
                        if (p >= limit)
                            return nullptr;
                        const uint8_t c = *p++;
                        v += c;
                        if (c != 255)
                            return v < (size_t(1) << 31) ? p : nullptr;
                    }
                };
                while (in < in_fast && op < op_fast)
                {
                    const uint8_t *const tok_start = in;
                    uint8_t *const op_start = op;
                    const size_t t = *in++;
                    const size_t s = t >> 7; // 1 = короткий токен
                    size_t lits = (t >> 3) & (7 >> s);
                    if (lits == 7)
                    {
                        const uint8_t *const q = fast_ext(in, in_end - 3, lits);
                        if (!q || op + lits > op_end - 32 || q + lits > in_end - 32)
                        {
                            in = tok_start;
                            op = op_start;
                            break;
                        }
                        in = q;
                        wildcopy32(op, in, op + lits);
                    }
                    else
                        std::memcpy(op, in, 16);
                    in += lits;
                    op += lits;
                    const size_t w = in[0] | (size_t(in[1]) << 8);
                    const size_t ofs_short = ((t << 3) & 0x300) | (w & 0xff);
                    const size_t ofs_long = w | ((t << 10) & 0x10000);
                    const size_t m = size_t(0) - s;
                    const size_t ofs = (ofs_short & m) | (ofs_long & ~m);
                    in += 2 - s;
                    const uint8_t *src = op - ofs;
                    size_t len = t & 0x7;
                    if (ofs == 0 || ofs > size_t(op - window))
                    {
                        in = tok_start;
                        op = op_start;
                        break;
                    }
                    if (len == 7)
                    {
                        const uint8_t *const q = fast_ext(in, in_end, len);
                        if (!q || op + len + ENCODE_MIN >= op_fast)
                        {
                            in = tok_start;
                            op = op_start;
                            break;
                        }
                        in = q;
                        len += ENCODE_MIN;
                    }
                    else
                    {
                        len += ENCODE_MIN;
                        if (ofs >= 8)
                        {
                            std::memcpy(op, src, 8);
                            std::memcpy(op + 8, src + 8, 8);
                            op += len;
                            continue;
                        }
                    }
                    uint8_t *const e = op + len;
                    if (ofs >= 16)
                        wildcopy32(op, src, e);
                    else if (ofs >= 8)
                        for (uint8_t *d = op; d < e; d += 8, src += 8)
                            std::memcpy(d, src, 8);
                    else
                        for (uint8_t *d = op; d < e; ++d)
                            *d = *(d - ofs);
                    op = e;
                }
            }

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
