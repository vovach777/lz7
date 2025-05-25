#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define MAX_OFFSET ((1 << 17)-1)
#define ENCODE_MIN (4)
#define BUCKET_N (32)
#define CHAIN_DISTANCE (8)
#define MAX_LEN (65535)
#define MAX_MATCH (MAX_LEN+ENCODE_MIN)
#define HASH_LOG2 (10)
#define HASH_SIZE (1 << HASH_LOG2)
#define LOOK_AHEAD (2)
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
                last = buffer[0] = val;
            } else {
                if (last != nullptr
                    && (
                            ( val[0]!=val[1] && last + 2 == val)
                            ||
                            ( val[0]==val[1] && last + 1 == val)
                        )
                    &&  std::distance(last, buffer[0]) < CHAIN_DISTANCE
                    )
                {
                    last = val; //keep bigest match ref until CHAIN_DISTANCE
                    return;
                }
        
                add(last=val);
            }
        }
        void add_if(const uint8_t* val) {
            //if (last && std::abs(std::distance(last, val)) < CHAIN_DISTANCE) return;
            auto it = std::find(buffer.begin(), buffer.end(), val);
            if (it == buffer.end()) {
                smart_add(val);
            } else {
                if (it != buffer.begin()) {
                    std::swap(*it, *(it-1));
                }
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

    struct Best{
        const uint8_t* ofs{};
        const uint8_t* ip2{};
        int len{};
        auto test_ofs() const {
            return std::distance( ofs+len , ip2);
        }
        auto test_len() const {
            return len - ENCODE_MIN;
        }
        void optimize(int literal_len, const uint8_t* ip) {

            if ( test_ofs() > MAX_OFFSET) {
                len = 0;
            }
            if (test_len() == ENCODE_MIN && test_ofs() == (MAX_OFFSET)) {
                //you ara lucky guys
                len = 0;
            }
            if (len < ENCODE_MIN) {
                len = 0;
            }
    
            literal_len -= std::distance(ip2, ip);
            // if (len == 2 && literal_len >= 15) {
            //     len = 0;
            // }
            // if (len == ENCODE_MIN && literal_len == 0) {
            //     len = 0;
            // }
            //euristic optimization
            // if (len < 4 && test_ofs() > 255 ) {
            //     len = 0;
            // } 

        }

    };

    // inline auto compare(const uint8_t* it1, const uint8_t* it2) const {
    //     auto [a,b] = std::mismatch(it1, data_end,  it2, it1);
    //     return std::distance(it1,a);
    // }
    uint16_t hash_of(const uint8_t* it) const {
        return static_cast<uint16_t>( reinterpret_cast<const uint32_t*>(it)[0] * 2654435761u >> (32-HASH_LOG2) );
    }


    public:
    TokenSearcher(const uint8_t* begin, const uint8_t* end, Put_function put) : put(put), idx(begin), ip(begin), data_begin(begin), data_end(end), tokens(HASH_SIZE) {}

    void tokenizer() {

        while (idx+4 <= ip) {
            auto& chain = tokens[hash_of(idx)];
            chain.smart_add(idx);
            idx++;
        }
    }


    void encode(const Best & best) {
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
            if (avail < ENCODE_MIN ) {
                literal_len+=avail;
                ip+=avail;
                put(MAX_OFFSET,ENCODE_MIN,(literal_len) ? ip - literal_len : nullptr, literal_len); //eof marker
                break;
            }

            tokenizer();
            auto best = search_best(ip);
            #ifdef LOOK_AHEAD
            for (int i = 1; i <= LOOK_AHEAD && avail-i >= ENCODE_MIN; ++i) {
                auto best2 = search_best(ip+i);
                if (best2.len > best.len) {
                    best = best2;
                } else
                   break;
            }
            
            #endif
            
            if (best.len < ENCODE_MIN) {
                literal_len++;
                ip++;
            } else {
                literal_len -= std::distance(best.ip2, ip);
                encode(best);
                ip = best.ip2 + best.len;
                literal_len = 0;
                // if (best.len > ENCODE_MIN*40) {
                
                //     auto chain = tokens[hash_of(best.ofs)];
                //     // chain.add_if(best.ofs);
                //     // rank-up!
                //     auto it = std::find(chain.buffer.begin(), chain.buffer.end(), best.ofs);
                //     if (it != chain.buffer.end()) {
                //         if (it != chain.buffer.begin())
                //             std::swap(*it, *(it-1));
                //     } else {
                //         //chain.add(best.ofs);
                //     }
                // }
                
            }
        }
        //put(0xffffff,MAX_MATCH,(literal_len) ? ip - literal_len : nullptr, literal_len);
    }
    Best search_best(const uint8_t* ip) const {
    
        
        auto literal_len = this->literal_len + std::distance(ip, this->ip);
        auto hash = hash_of(ip);
        auto& chain = tokens[hash];
      
        Best best{};

        for (int i = 0; i < BUCKET_N; ++i)
        {
            auto it = chain.buffer[i];
            if (it == nullptr)
                break;


            // if (reinterpret_cast<const uint32_t*>(it)[0] != reinterpret_cast<const uint32_t*>(ip)[0])
            // {
            //     continue;
            // }
            // if ( std::distance(it, ip) < ENCODE_MIN )
            // {
            //     continue;
            // }
            auto ip_lim = std::min(ip+MAX_LEN, data_end);

            auto [ it_mismatch, ip_mismatch ] = std::mismatch(it, ip, ip, ip_lim);
            int match = std::distance(it, it_mismatch);
            if (match < ENCODE_MIN) {
                continue;
            }
            assert( match < MAX_MATCH);
            //back matching
            auto ip2 = ip;
            auto back_len = 0;
   
            while ( match < MAX_MATCH && back_len < literal_len &&  it[-1] == ip2[-1] ) {
                ++back_len;
                --it;
                --ip2;
                if (it + match != ip2)
                    ++match;
            }

            Best opt = {it, ip2, match};
            opt.optimize(literal_len, ip);

            if (opt.len && ( opt.len > best.len || (opt.len == best.len && it > best.ofs))) {
                best = opt;             
            } else
            if ( best.len >= 31 /* big enough*/ ) {
                break;
            }
        }

        return best;
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
