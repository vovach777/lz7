#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define _USE_SIMD
#define MAX_OFFSET ((1 << 17)-1)
#define NO_MATCH_OFS (MAX_OFFSET)
#define ENCODE_MIN (4)
#define MAX_ITER_SCAN (16)
#define MAX_LEN (65535)
#define MAX_MATCH (MAX_LEN+ENCODE_MIN)
#define HASH_LOG2 (16)
#define HASH_SIZE (1 << HASH_LOG2)
#define CHAIN_LOG2 (16)
#define CHAIN_SIZE (1 << CHAIN_LOG2)
#define CHAIN_BREAK (CHAIN_SIZE - 1)
#define LOOK_AHEAD (3)
#define MICRO_HASH (8)
#define RLE_INDEX_TRIGGER 8
//#define _USE_NEXT_MATCH_OPTIMIZATION
#define _USE_FAST_SKIP
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

using Put_function = std::function<void(int offset, int len, const uint8_t * literals, int literals_len )>;

inline bool is_rle(const uint8_t* r) {

    return std::memcmp(r, r+1, ENCODE_MIN-1) == 0;

}

inline int ilog2(unsigned v) {
    int targetlevel = 1;
    while (v>>=1) ++targetlevel;
    return targetlevel;
}

class TokenSearcher {
    struct HashItem
    {
        const uint8_t* idx{nullptr};
        uint16_t next_item{CHAIN_BREAK};
        //uint16_t offset_relative{0}; //is allways 0. idx = idx-0;
    };
    struct ChainItem
    {
        uint16_t next_item{CHAIN_BREAK};
        uint16_t offset_relative{0xFFFF};
    };

    std::vector<HashItem> hashtabele; //index to chain
    std::vector<ChainItem> chaintable;
    uint16_t next_override_item{0};

    void register_chain_item(const uint8_t* idx) {
        if (idx+ENCODE_MIN>=data_end) return;
        auto hash = hash_of(idx);
        //в хэш-таблице ссылка на следующий элемент в цепочке. там находится размется между текущем смещением и текущим.
        auto& hash_item = hashtabele[hash];
        if (hash_item.idx == nullptr) {
            hash_item.idx = idx;
            hash_item.next_item = CHAIN_BREAK;
            return;
        }
        if ( hash_item.idx >= idx ) {
            return;
        }
        uint16_t id = next_override_item;
        if (id+1 == CHAIN_BREAK)
            next_override_item = 0; //round
        else
            next_override_item = id+1;

        auto& chain_item = chaintable[id];
        chain_item.next_item = hash_item.next_item;
        chain_item.offset_relative = std::min<ptrdiff_t>(0xFFFF, idx - hash_item.idx);
        hash_item.idx = idx;
        hash_item.next_item = id;
    }


    void index() {

        while (idxp < ip) {
            if (idxp == force_index_rle) {
                force_index_rle = nullptr;
                register_chain_item(idxp);
                idxp += match_len_simd(idxp, idxp + 1, data_end) + 1;
            }
            else
            if (idxp != force_index_rle &&  is_rle(idxp) && !no_collision(idxp)) {
                //std::cerr << "RLE-skip-index: " << std::string_view((const char*)idxp, ENCODE_MIN) <<  std::endl;
                idxp += match_len_simd(idxp, idxp + 1, data_end) + 1;
            } else {
                register_chain_item(idxp);
                idxp++;
            }
        }
    }

    const uint8_t* data_begin;
    const uint8_t* data_end;
    const uint8_t* ip;
    const uint8_t* idxp;
    const uint8_t* emitp;
    Put_function put;

    struct Best{
        const uint8_t* ofs{};
        const uint8_t* ip2{};
        int len{};
        int gain{std::numeric_limits<int>::min()};
        bool is_self_reference(const TokenSearcher& ctx) const {
            return ofs + len > ctx.emitp;
        }
        int self_reference_count(const TokenSearcher& ctx) const {
            return std::max<ptrdiff_t>(0, (ofs+len) - ctx.emitp);
        }
        auto test_ofs() const {
            return std::distance( ofs , ip2);
        }
        auto test_short() const {
            return test_ofs() < (1<<10);
        }

        size_t intersect_len(const uint8_t* first1, const uint8_t* last1,
            const uint8_t* first2, const uint8_t* last2) const {
            // Находим потенциальное начало пересечения
            // Это наибольший из двух начальных указателей
            const uint8_t* intersection_first = std::max(first1, first2);

            // Находим потенциальный конец пересечения
            // Это наименьший из двух конечных указателей
            const uint8_t* intersection_last = std::min(last1, last2);

            // Если начало пересечения меньше конца, значит пересечение существует
            if (intersection_first < intersection_last) {
                return std::distance(intersection_first, intersection_last);
            } else {
                // В противном случае пересечения нет, возвращаем (last1, last1)
                return 0;
            }
        }

        void set_to_literals(const TokenSearcher& ctx) {
             auto literal_len = std::distance(ctx.emitp,ip2);
             len = 0;
             gain = -2 /*cost*/ - literal_len - (literal_len + 255 - 31) / 255;
        }
        void optimize(const TokenSearcher& ctx) {
            assert(ip2 >= ctx.emitp);
            if (len < ENCODE_MIN || test_ofs() > MAX_OFFSET) {
                set_to_literals(ctx);
                return;
            }
            if (len == ENCODE_MIN+7) {
                len--;
                //std::cerr << "optimzation len==11 is useless" << std::endl;
            }
            assert(test_ofs() > 0);
            auto literal_len = std::distance(ctx.emitp,ip2);
            assert(literal_len >= 0);
            // euristic optimization
            int cost;
            if (test_short() && literal_len <= 3) {
                cost =  2 + match_cost(len);
            } else
                cost = 3 + literal_cost(literal_len) + match_cost(len);
            gain = len - cost - literal_len;

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

    inline uint16_t hash_of(const uint8_t* it) {
        #if ENCODE_MIN == 3
        return static_cast<uint16_t>( (reinterpret_cast<const uint32_t*>(it)[0]&0xffffff) * 2654435761u >> (32-HASH_LOG2) );
        #else
        return static_cast<uint16_t>( reinterpret_cast<const uint32_t*>(it)[0] * 2654435761u >> (32-HASH_LOG2) );
        #endif
    }

    template <size_t N>
    inline uint16_t hash_of(const uint8_t* it) {
        #if ENCODE_MIN == 3
        return static_cast<uint16_t>( (reinterpret_cast<const uint32_t*>(it)[0]&0xffffff) * 2654435761u >> (32-N) );
        #else
        return static_cast<uint16_t>( reinterpret_cast<const uint32_t*>(it)[0] * 2654435761u >> (32-N) );
        #endif
    }


    public:
    TokenSearcher(const uint8_t* begin, const uint8_t* end, Put_function put) : put(put), idxp(begin), ip(begin), emitp(begin), data_begin(begin), data_end(end), hashtabele(HASH_SIZE,HashItem{}), chaintable(CHAIN_SIZE,ChainItem{}) {}


    void emit(const Best & best) {

        assert(best.ip2 >= emitp);
        assert(best.ofs <  best.ip2);
        if (best.len < ENCODE_MIN) {
            emit(); //emit as literal
            return;
        }
        put(best.test_ofs(), best.len, emitp, best.ip2 - emitp);
        emitp = ip = best.ip2 + best.len;
    }

    void emit() {
        assert(ip >= emitp);
        if (ip > emitp) {
            put(0, 0, emitp, ip - emitp);
            emitp = ip;
        }
    }


    void compress() {
        ip+=1;
        if (ip > data_end) {
            ip = data_end;
        };
        for (;;)
        {
            if (ip + ENCODE_MIN > data_end){
                emit();
                break;
            }
            Best match = search_best();
            if (match.len < ENCODE_MIN) {
                ip++;
                continue;
            }
#ifdef _USE_NEXT_MATCH_OPTIMIZATION
            if (!match.is_self_reference(*this) &&  match.test_short() && std::distance(emitp,match.ip2) <= 3) {
                //try more aggresive
                ip = emitp + 3;
                auto next_match = search_best();
                if (next_match.gain > match.gain) {
                    match = next_match;
                }
            } else
            if (!match.is_self_reference(*this) &&   std::distance(emitp,match.ip2) <= 6) {
                //try more aggresive
                ip = emitp + 6;
                auto next_match = search_best();
                if (next_match.gain > match.gain) {
                    match = next_match;
                }
            }

#else

#if LOOK_AHEAD > 0
#if MICRO_HASH > 0
            alignas(32) std::array<bool, 1 << MICRO_HASH> candidates;
            std::fill(candidates.begin(), candidates.end(), false);
            candidates[ hash_of<MICRO_HASH>(ip) ] = true;
#else
            std::unordered_set<uint16_t> candidates;
            candidates.reserve(4);
            candidates.insert(hash_of(ip));
#endif

            //fast-skip
            for (ip+=1; ip < emitp+LOOK_AHEAD; ip += 1)
            {
#if MICRO_HASH > 0
                const uint16_t hash = hash_of<MICRO_HASH>(ip);
                if (candidates[hash])
                    break;
                candidates[hash] = true;
#else
                if ( candidates.count(hash_of(ip)) != 0 )
                    break;
                candidates.insert(hash_of(ip));
#endif
                if (no_collision(ip))
                    continue;
                auto next_match = search_best();
                if (next_match.gain > match.gain) {
                    match = next_match;
                }
            }
#endif
#endif
            emit(match);
        }

    }

#ifdef _USE_SIMD
    inline int match_len_simd(const uint8_t* a, const uint8_t* b, const uint8_t* a_end) {
        int len = 0;
        while (a + 16 <= a_end) {
            __m128i va = _mm_loadu_si128((__m128i*)a);
            __m128i vb = _mm_loadu_si128((__m128i*)b);
            __m128i cmp = _mm_cmpeq_epi8(va, vb);
            int mask = _mm_movemask_epi8(cmp);
            if (mask != 0xFFFF) {
                return len + __builtin_ctz(~mask); // первая разница
            }
            len += 16;
            a += 16;
            b += 16;
        }
        // добить хвост
        while (a < a_end && *a == *b) {
            ++a; ++b; ++len;
        }
        return len;
    }

    inline int match_len_simd_backward(const uint8_t* a_begin, const uint8_t* a, const uint8_t* b_begin, const uint8_t* b) {
        int len = 0;
        while (a - 15 >= a_begin && b - 15 >= b_begin) {
            __m128i va = _mm_loadu_si128((const __m128i*)(a - 15));
            __m128i vb = _mm_loadu_si128((const __m128i*)(b - 15));
            __m128i cmp = _mm_cmpeq_epi8(va, vb);
            int mask = _mm_movemask_epi8(cmp);

            if (mask != 0xFFFF) {
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

        while (a >= a_begin && b >= b_begin && *a == *b) {
            --a;
            --b;
            ++len;
        }

        return len;
    }
    #endif

    const uint8_t* force_index_rle{nullptr};
    inline bool no_collision(const uint8_t* idx) {
        idx =  hashtabele[hash_of(idx)].idx;//idx to hashed-idx =)
        return idx==nullptr || (emitp-idx) > MAX_OFFSET;
    }


    Best search_best() {
        Best best{};
        uint16_t hash;
        const uint8_t* idx;
        int next_item;
        auto nbAttemptsMax{MAX_ITER_SCAN};

        index();

        if (is_rle(ip)) {
            auto len = match_len_simd(ip+1, ip, data_end)+1;
            auto backlen = match_len_simd_backward(data_begin,ip-1,emitp, ip);
            auto rle_len = len + backlen;
            auto rle_ofs = ip - backlen;
            //1: index this RLE if it is long enough or if there is no hash collision
            if (rle_len >= RLE_INDEX_TRIGGER /*|| (rle_len >= ENCODE_MIN && no_collision(rle_ofs))*/) {
                force_index_rle = rle_ofs;
            }

            //2: create RLE match (self-reference match)
            best.ofs = rle_ofs;
            best.ip2 = rle_ofs + 1;
            best.len = rle_len - 1;
            best.optimize(*this);

        }
        #ifdef _USE_FAST_SKIP
        else {
            auto fast_skip_len = 0;
            while (ip+ENCODE_MIN <= data_end && no_collision(ip)) {
                ip++;
                index();
                fast_skip_len++;

            }
            // if (fast_skip_len > 0) {
            //      std::cerr << "fast skip: " << std::string_view((const char*)ip-fast_skip_len, fast_skip_len) << std::endl;
            // }
            if ( ip+ENCODE_MIN > data_end )
                return best;
        }
        #endif

        for (int nbAttempts=0; nbAttempts != nbAttemptsMax; nbAttempts++)
        {
            if (nbAttempts == 0) {
                hash = hash_of(ip);
                auto  & hash_item = hashtabele[hash];
                idx = hash_item.idx;
                if (idx == nullptr)
                    break;
                next_item = hash_item.next_item;
            } else {
                if (next_item == CHAIN_BREAK)
                    break;
                auto  & chain_item = chaintable[next_item];
                // if ( chain_item.offset_relative == 0xFFFF) {
                //     break;
                // }
                idx -= chain_item.offset_relative;
                if (idx < data_begin)
                    break;
                if (hash_of(idx) != hash)
                    break;
                next_item = chain_item.next_item;
            }
            //pre-hashing happens...
            //fast skip
            if (idx >= ip)
                continue;
#ifdef _USE_SIMD
            auto  match_len = match_len_simd(ip, idx, data_end);
#else
            auto [ it_mismatch, ip_mismatch ] = std::mismatch(it, data_end, ip, data_end);
            int match_len = std::distance(it, it_mismatch);
#endif

            if (match_len < ENCODE_MIN)
                continue;
            // if (it_mismatch > ip) {
            //   //  std::cerr << "self-ref: " <<  it_mismatch-ip <<   std::endl;
            // }
#ifdef _USE_SIMD
            int back_match_len = match_len_simd_backward(data_begin, idx-1, emitp,  ip-1);
            auto ip2 = ip - back_match_len;
            idx  -= back_match_len;
            match_len += back_match_len;
#else
            //back matching
            auto ip2 = ip;
            while (  it > data_begin  &&  ip2 > emitp  &&  it[-1] == ip2[-1] ) {
                --it;
                --ip2;
                ++match_len;
            }
#endif
            assert(ip2 >= emitp);
            assert(idx >= data_begin);
            //assert(it + match <= ip2);

            Best after{idx, ip2, match_len };
            after.optimize(*this);

            if (after.gain > best.gain) {
                best = after;
                //std::cerr << " gain search step: " << (best.gain - after.gain) << std::endl;
            }
        }
    return best;
    }
};


inline void lz_comp(const uint8_t*  begin, const uint8_t*  end, Put_function put) {
    auto current = begin;
    int plain_len = 0;
    auto ts = TokenSearcher(begin, end,put);
    ts.compress();
}


#endif // LZ7_H
