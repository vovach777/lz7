#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define MAX_OFFSET ((1 << 17)-1)
#define ENCODE_MIN (3)
#define CHAIN_DISTANCE (0)
#define MAX_FALSE_SEQUENCE_COUNT (12)
#define MAX_LEN (65535)
#define MAX_MATCH (MAX_LEN+ENCODE_MIN)
#define HASH_LOG2 (15)
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
    struct ChainItem
    {
        const uint8_t* pos{nullptr};
        uint16_t next{0xffff};
    };

    std::vector<uint16_t> hashtabele; //index to chain
    std::vector<ChainItem> chain;
    uint16_t next_override_item = 0;

    uint16_t extract_chain_item() {
        uint16_t id = next_override_item;
        if (id == 0xfffe)
            next_override_item = 0; //round
        else
            next_override_item = id+1;

        ChainItem& old_item = chain[id];
        //not empty item
        if (old_item.pos != nullptr)  {
            auto& old_index_ref = hashtabele[hash_of(old_item.pos)];
            if (old_index_ref == id) //was root
                old_index_ref = 0xffff; //delete ref to it
        }
        //clear
        old_item.pos = nullptr;
        old_item.next = 0xffff;
        return id;
    }
    void index(const uint8_t* ip) {
        while (idx+ENCODE_MIN <= ip) {
            auto id = extract_chain_item();
            auto& item = chain[id];
            auto& index_ref = hashtabele[hash_of(idx)];
            item.pos = idx;
            item.next = index_ref;
            index_ref = id;
            idx += 1;
        }
    }

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
    TokenSearcher(const uint8_t* begin, const uint8_t* end, Put_function put) : put(put), idx(begin), ip(begin), data_begin(begin), data_end(end), hashtabele(HASH_SIZE,0xffff), chain(0x10000,ChainItem{})   {}


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
            int step = std::min<ptrdiff_t>(avail,std::max(LOOK_AHEAD+1,1));

            if ( best.len < ENCODE_MIN || best.gain >= literal_len+step) {
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
        int better_count = 0;
        auto ip = this->ip;
        int better_hash = -1;
        auto ip_lim  = std::min(this->ip+LOOK_AHEAD, data_end-ENCODE_MIN);

        for (auto ip = this->ip; ip <= ip_lim; ip++ )
        {
            auto chain_breaker = hash_of(ip);
            if (chain_breaker == better_hash) {
                continue;
            }

            index(ip);
            for (int id = hashtabele[hash_of(ip)], false_sequence_count=0; false_sequence_count < MAX_FALSE_SEQUENCE_COUNT &&  id != 0xffff; id = chain[id].next, false_sequence_count++)
            {
                auto it = chain[id].pos;
                assert(it != nullptr);

                if (hash_of(it) != chain_breaker) {
                    break;
                }


                if (std::distance(it,ip) > MAX_OFFSET) {
                    break;
                }


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

                if (after.is_better_than(best)) {
                    best = after;
                    better_count++;
                    better_hash = hash_of(ip);
                }
                // else
                //    if ( better_count > 4 )
                //        return best;

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
