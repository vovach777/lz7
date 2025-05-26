#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define MAX_OFFSET ((1 << 17)-1)
#define ENCODE_MIN (3)
#define BUCKET_N (16)
#define CHAIN_DISTANCE (0)
#define MAX_LEN (65535)
#define MAX_MATCH (MAX_LEN+ENCODE_MIN)
#define HASH_LOG2 (13)
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
        uint8_t pos{0};
        inline void add(const uint8_t* val){
            buffer[pos] = val;
            if (++pos == BUCKET_N)
                pos = 0;
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
        int gain{0x8000};
        auto test_ofs() const {
            return std::distance( ofs+len , ip2);
        }



        void optimize(int literal_len_orig, const uint8_t* ip) {

            if (len < ENCODE_MIN || test_ofs() > MAX_OFFSET) {
                len = 0;
                return;
            }
            // euristic optimization

            if (len == ENCODE_MIN && test_ofs() == (MAX_OFFSET)) {
                //you ara lucky guys
                len = 0;
                return;
            }

            auto literal_len = std::distance(ip-literal_len_orig,ip2);

            if (test_ofs() < (1<<10) && literal_len <= 3) {
                gain = literal_len + 2 + match_cost(len) - len;
                return;
            }
            gain = literal_len + 3 + literal_cost(literal_len) + match_cost(len) - len;
        }

        static int literal_cost(unsigned long nlit)
        {
            return (nlit + 255 - 7) / 255;
        }

        static int match_cost(unsigned long len)
        {
            return (len + 255 - ENCODE_MIN - 7) / 255;
        }

        bool is_better_than(const Best& other) const {
            return gain < other.gain || (gain == other.gain && ip2 < other.ip2);
        }

    };

    // inline auto compare(const uint8_t* it1, const uint8_t* it2) const {
    //     auto [a,b] = std::mismatch(it1, data_end,  it2, it1);
    //     return std::distance(it1,a);
    // }
    uint16_t hash_of(const uint8_t* it) const {
        #if ENCODE_MIN == 3
        return static_cast<uint16_t>( (reinterpret_cast<const uint32_t*>(it)[0]&0xffffff) * 2654435761u >> (32-HASH_LOG2) );
        #else
        return static_cast<uint16_t>( reinterpret_cast<const uint32_t*>(it)[0] * 2654435761u >> (32-HASH_LOG2) );
        #endif
    }


    public:
    TokenSearcher(const uint8_t* begin, const uint8_t* end, Put_function put) : put(put), idx(begin), ip(begin), data_begin(begin), data_end(end), tokens(HASH_SIZE) {}

    void tokenizer(const uint8_t* ip) {

        while (idx+ENCODE_MIN <= ip) {
            auto& chain = tokens[hash_of(idx)];
            chain.add(idx);
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
            if (avail < ENCODE_MIN+LOOK_AHEAD ) {
                literal_len+=avail;
                ip+=avail;
                put(MAX_OFFSET,ENCODE_MIN,(literal_len) ? ip - literal_len : nullptr, literal_len); //eof marker
                break;
            }

            auto best = search_best();
            int step = std::min<ptrdiff_t>(avail,std::max(LOOK_AHEAD/2,1));

            if ( best.len < ENCODE_MIN || best.gain > literal_len+step) {
                literal_len+=step;
                ip+=step;
            } else {


                literal_len -= std::distance(best.ip2, ip);
                encode(best);
                ip = best.ip2 + best.len;
                literal_len = 0;


            }
        }
        //put(0xffffff,MAX_MATCH,(literal_len) ? ip - literal_len : nullptr, literal_len);
    }
    Best search_best()  {
        Best best{};
        for (auto ip=this->ip; ip <= this->ip+LOOK_AHEAD; ++ip)
        {
            //auto literal_len = this->literal_len + std::distance(ip, this->ip);
            auto hash = hash_of(ip);
            auto& chain = tokens[hash];
            tokenizer(ip);

            for (int i = 0; i < BUCKET_N; ++i)
            {
                auto it = chain.buffer[i];
                if (it == nullptr )
                    break;
                if (it+ENCODE_MIN > ip) {
                    continue;
                }

                auto ip_lim = std::min(ip+MAX_LEN, data_end);

                auto [ it_mismatch, ip_mismatch ] = std::mismatch(it, ip, ip, ip_lim);
                int match = std::distance(it, it_mismatch);
                if (match < ENCODE_MIN) {
                    continue;
                }
                assert( match < MAX_MATCH);
                //back matching
                auto ip2 = ip;

                auto back_lim = this->ip-this->literal_len;
                while ( match < MAX_MATCH && ip2 > back_lim  &&  it[-1] == ip2[-1] ) {
                    --it;
                    --ip2;
                    if (it + match != ip2)
                        ++match;
                }

                Best after{it, ip2, match };
                after.optimize(this->literal_len, this->ip);



                if (after.is_better_than(best))
                    best = after;

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
