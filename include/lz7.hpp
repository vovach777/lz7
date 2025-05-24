#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define WINDOW_N (1 << 24)
#define ENCODE_MIN (4)
#define BUCKET_N (16)
#define CHAIN_DISTANCE (8)
#define MAX_LEN (65535)
#define MAX_MATCH (MAX_LEN+ENCODE_MIN)
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
#include <tuple>
#include <array>

namespace lz7 {


#define HAS_BUILTIN_CLZ
inline int ilog2_32(uint32_t v, int error_value = 0) {
    if (v == 0) return error_value;
#ifdef HAS_BUILTIN_CLZ
    return 31 - __builtin_clz(v);
#else
    return static_cast<uint32_t>(std::log2(v));
#endif
}

}

using Put_function = std::function<void(int offset, int len, const uint8_t * literals, int literals_len )>;
class TokenSearcher {
    struct Item
    {
        std::array<const uint8_t*,BUCKET_N> buffer;
        const uint8_t* last{nullptr};
        inline void add(const uint8_t* val){
            std::memmove(&buffer[1], &buffer[0], (BUCKET_N-1) * sizeof(uint8_t*));
            buffer[0] = val;
        }
        inline void smart_add(const uint8_t* val){
            if (buffer[0] == nullptr) {
                buffer[0] = val;
            } else {
                if (last != nullptr
                    && (
                            ( val[0]!=val[1] && last + 2 == val)
                            ||
                            ( val[0]==val[1] && last + 1 == val)
                        )
                    )
                {
                    auto replace_pos = std::find(buffer.begin(), buffer.end(), last);
                    if (replace_pos != buffer.end()) {
                        last = *replace_pos = val;
                        return;
                    }
                }
                add(last=val);
            }
        }
        Item() {
            std::fill(buffer.begin(), buffer.end(), nullptr);
        }

    };

    std::vector<Item> tokens; //<Byte <Item> tokens;
    const uint8_t* data_begin;
    const uint8_t* data_end;
    const uint8_t* ip;
    const uint8_t* idx;
    int literal_len = 0;
    Put_function put;

    struct {
        const uint8_t* ofs{};
        const uint8_t* ip2{};
        int len{};
        auto test_ofs() {
            return std::distance( ofs+len , ip2);
        }
        auto test_len() {
            return len - ENCODE_MIN;
        }
        // void optimize(int literal_len, const uint8_t* ip) {

        //     literal_len -= std::distance(ip2, ip);
        //     //euristic optimization
        //     if (len == 3 &&  literal_len == 0 && test_ofs() < 64 ) {
        //         len = 2;
        //     } else
        //     if (len == 3 &&  literal_len == 0 && test_ofs() > 127 ) {
        //         //revert to literal
        //         len = 0;
        //     }

        //     if (len == 3 && literal_len > 0) {
        //         len = 0;
        //     }

        //     if (len == 2) {
        //         if  (literal_len>0)
        //            len = 0;
        //         else
        //         if (test_ofs() > 127 )
        //            len = 0;
        //     }
        // }
        void optimize(int literal_len, const uint8_t* ip) {

            if ( test_ofs() > 65535) {
                len = 0;
            }
            if (len < ENCODE_MIN) {
                len = 0;
            }
    
            literal_len -= std::distance(ip2, ip);
            if (len == ENCODE_MIN && literal_len >= 15) {
                len = 0;
            }
            // if (len == ENCODE_MIN && literal_len == 0) {
            //     len = 0;
            // }
            //euristic optimization
            // if (len < 4 && test_ofs() > 255 ) {
            //     len = 0;
            // } 

        }

    } best{};

    // inline auto compare(const uint8_t* it1, const uint8_t* it2) const {
    //     auto [a,b] = std::mismatch(it1, data_end,  it2, it1);
    //     return std::distance(it1,a);
    // }
    uint16_t hash_of(const uint8_t* it) const {
        return reinterpret_cast<const uint16_t*>(it)[0];
    }


    public:
    TokenSearcher(const uint8_t* begin, const uint8_t* end, Put_function put) : put(put), idx(begin), ip(begin), data_begin(begin), data_end(end), tokens(0x10000) {}

    void tokenizer() {

        while (idx+1 < ip) {
            auto& chain = tokens[hash_of(idx)];
            chain.smart_add(idx);
            idx++;
        }
    }


    void encode() {
        put(best.test_ofs(), best.len, best.ip2 - literal_len, literal_len);
    }

    void compressor() {

        for (;;)
        {
            if (ip == data_begin){
                literal_len=ENCODE_MIN;
                ip+=ENCODE_MIN;
            }
            auto avail = std::distance(ip, data_end);
            if (avail < ENCODE_MIN*2 ) {
                literal_len+=avail;
                break;
            }

            tokenizer();
            search_best();
            if (best.len < 2) {
                literal_len++;
                ip++;
            } else {
                literal_len -= std::distance(best.ip2, ip);
                encode();
                ip = best.ip2 + best.len;
                literal_len = 0;
            }
        }
        //put(0xffffff,MAX_MATCH,(literal_len) ? ip - literal_len : nullptr, literal_len);
    }
    void search_best()  {
        auto& chain = tokens[hash_of(ip)];
        best = {};

        for (int i = 0; i < BUCKET_N; ++i)
        {
            auto it = chain.buffer[i];
            if (it == nullptr)
                break;
            if ( std::distance(it, ip) > WINDOW_N )
            {
                continue;
            }

            assert(ip[0] == it[0]);
            assert(ip[1] == it[1]);


            auto max_forward = std::distance(it+2, ip);
            if (max_forward > MAX_LEN)
                max_forward = MAX_LEN;

            if ( max_forward <  0 ) {
                continue;
            }

            auto [ it_mismatch, ip_mismatch ] = std::mismatch(it+2, ip, ip+2, ip+MAX_LEN);
            int match = std::distance(it, it_mismatch);
            assert( match < MAX_MATCH);
            //back matching
            auto ip2 = ip;
            auto back_len = 0;
            auto it_end = it + match;

            while ( match < MAX_MATCH && back_len < literal_len &&  it[-1] == ip2[-1] ) {
                ++back_len;
                --it;
                --ip2;
                if (it + match != ip2)
                    ++match;
            }

            decltype(best) opt = {it, ip2, match};
            opt.optimize(literal_len, ip);

            if (opt.len && ( opt.len > best.len || (opt.len == best.len && it > best.ofs))) {
                best = opt;
                if (i > 0 && best.len > ENCODE_MIN) {
                    std::swap(chain.buffer[i], chain.buffer[i-1]);
                }
            } else
            if ( best.len >= 31 /* big enough*/ ) {
                break;
            }
        }
    }
};

template <typename It, typename PutSymbol>
inline void lz_comp(It begin, It end, PutSymbol&& put) {
    auto current = begin;
    int plain_len = 0;
    auto ts = TokenSearcher(begin, end,put);
    ts.compressor();
}


#endif // LZ7_H
