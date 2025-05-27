#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define MAX_OFFSET ((1 << 17)-1)
#define ENCODE_MIN (3)
#define CHAIN_DISTANCE (0)
#define MAX_FALSE_SEQUENCE_COUNT (32)
#define MAX_LEN (65535)
#define MAX_MATCH (MAX_LEN+ENCODE_MIN)
#define HASH_LOG2 (16)
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
    const uint8_t* emitp;
    Put_function put;

    struct Best{
        const uint8_t* ofs{};
        const uint8_t* ip2{};
        int len{};
        int gain{0x8000};
        auto test_ofs() const {
            return std::distance( ofs+len , ip2);
        }



        void optimize(const TokenSearcher& ctx) {

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

            auto literal_len = std::distance(ctx.emitp,ip2);

            if (test_ofs() < (1<<10) && literal_len <= 3) {
                gain = literal_len +  2 + match_cost(len) - len;
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
    TokenSearcher(const uint8_t* begin, const uint8_t* end, Put_function put) : put(put), idx(begin), ip(begin), emitp(begin), data_begin(begin), data_end(end), hashtabele(HASH_SIZE,0xffff), chain(0x10000,ChainItem{})   {}


    void emit(const Best & best) {
        assert(best.ip2 >= emitp);
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
            put(0x1ffff, ENCODE_MIN, emitp, ip - emitp);
            emitp = ip;
        }
    }

    void compressor() {

        for (;;)
        {
            if (ip == data_begin){
                ip+=ENCODE_MIN;
            }
            auto avail = std::distance(ip, data_end);
            if (avail < ENCODE_MIN ) {
                ip=data_end;
                emit();
                break;
            }

            auto greedy_best_match = search_best(ip);
            if ( greedy_best_match.len < ENCODE_MIN) {
                ip++;
                continue;
            }
            if (greedy_best_match.gain >= -1 &&
                (greedy_best_match.test_ofs() < (1<<10) &&  greedy_best_match.ip2 - emitp == 3)
                || (greedy_best_match.ip2 - emitp == 6 ))
            {
                emit(greedy_best_match);
                continue; //keep literals low

            }

            // auto greedy_best_match_next_step = search_best(ip+1);
            // if ( greedy_best_match_next_step.gain >= greedy_best_match.gain) {
            //     emit(greedy_best_match);
            //     continue;
            //     //greedy_best_match = greedy_best_match_next_step;
            //     //assert(greedy_best_match.len >= ENCODE_MIN);

            // }
            // greedy_best_match = greedy_best_match_next_step;


            // if ( greedy_best_match.gain < -4) {
            //     /* not realy hiding realy good next match... acceptobly! */
            //     emit(greedy_best_match);
            //     continue;
            // }
            //check if can split greedy match with lazy
            Best lazy_best_match = greedy_best_match;
            for (int i = LOOK_AHEAD; i >= 1; --i) {
                auto lazy_match = search_best(ip+i);
                if (lazy_match.gain <= lazy_best_match.gain) {
                    lazy_best_match = lazy_match;
                    if (lazy_best_match.ip2 <= greedy_best_match.ip2) {
                        break;
                    }

                }
            }

            //check if hide short match previously finded
            #if 0
            //experiment: split agreed match instanded of revert to literals - no luck!
            if (greedy_best_match.ip2 < lazy_best_match.ip2 )  {

                auto intersection = greedy_best_match.ip2 + greedy_best_match.len - lazy_best_match.ip2;
                if ( intersection < 0 && greedy_best_match.gain < 0) {
                    emit(greedy_best_match);
                    continue;
                } else
                if (intersection >= 0 && greedy_best_match.len-intersection >= ENCODE_MIN) {
                    greedy_best_match.len -= intersection;
                    greedy_best_match.optimize(*this);
                    if (greedy_best_match.len >= ENCODE_MIN && greedy_best_match.gain < 0)
                        emit(greedy_best_match);
                }
            }
            #endif
            emit(lazy_best_match);



        }

    }
    Best search_best( const uint8_t* ip) {
        Best best{};
        // int better_count = 0;
        // int better_hash = -1;
        auto chain_breaker = hash_of(ip);

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

            while ( match < MAX_MATCH && ip2 > emitp  &&  it[-1] == ip2[-1] ) {
                --it;
                --ip2;
                if (it + match != ip2)
                    ++match;
            }

            Best after{it, ip2, match };
            after.optimize(*this);

            if (after.is_better_than(best)) {
                best = after;
                // better_count++;
                // better_hash = hash_of(ip);
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
