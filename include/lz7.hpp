#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define MAX_OFFSET ((1 << 17)-1)
#define NO_MATCH_OFS (MAX_OFFSET)
#define ENCODE_MIN (3)
#define MAX_FALSE_SEQUENCE_COUNT (32)
#define MAX_LEN (65535)
#define MAX_MATCH (MAX_LEN+ENCODE_MIN)
#define HASH_LOG2 (16)
#define HASH_SIZE (1 << HASH_LOG2)
#define CHAIN_LOG2 (16)
#define CHAIN_SIZE (1 << CHAIN_LOG2)
#define CHAIN_BREAK (CHAIN_SIZE - 1)
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
#include <tuple>
#include <array>
#include <optional>
using Put_function = std::function<void(int offset, int len, const uint8_t * literals, int literals_len )>;
class TokenSearcher {
    struct ChainItem
    {
        const uint8_t* pos{nullptr};
        uint16_t next{CHAIN_BREAK};
        uint16_t hash{};
    };

    std::vector<uint16_t> hashtabele; //index to chain
    std::vector<ChainItem> chain;
    uint16_t next_override_item{0};

    void register_chain_item(const uint8_t* idx) {
        uint16_t id = next_override_item;
        if (id+1 == CHAIN_BREAK)
            next_override_item = 0; //round
        else
            next_override_item = id+1;

        ChainItem& item = chain[id];
        //not empty item
        if (item.pos != nullptr)  {
            if (hashtabele[item.hash] == id)
                hashtabele[item.hash] = CHAIN_BREAK; //unregister
        }
        //clear
        item.pos = idx;
        item.hash = hash_of(idx);
        item.next = hashtabele[item.hash];
        hashtabele[item.hash] = id;
    }
    int current_idx_rle = 0;
    void index(const uint8_t* ip) {

        while (idx+ENCODE_MIN <= ip) {
            if ( idx[0] == idx[+1]) {
                if ( ((current_idx_rle -1) & current_idx_rle) == 0) {
                    if (current_idx_rle == 0 || current_idx_rle > 4) {
                        // register_chain_item(idx);
                        // if (current_idx_rle >= 8)
                        //     std::cout << "RLE: " << std::string_view((const char*)idx-current_idx_rle, std::min(16, current_idx_rle) ) << "/" <<  current_idx_rle << std::endl;
                    }
                }
                current_idx_rle += 1;

            } else {

                register_chain_item(idx);
                current_idx_rle = 0;

            }
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
        int gain{MAX_OFFSET};
        bool is_self_reference(const TokenSearcher& ctx) const {
            return ofs + len > ctx.emitp;
        }
        int self_reference_count(const TokenSearcher& ctx) const {
            return std::max<ptrdiff_t>(0, (ofs+len) - ctx.emitp);
        }
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
            int thelen = len;

            auto literal_len = std::distance(ctx.emitp,ip2);
            auto literal_match_part = std::distance(ofs+len,ctx.emitp);
            if (literal_match_part < 0) {
                thelen += literal_match_part;
            }

            if (test_ofs() < (1<<10) && literal_len <= 3) {
                gain = literal_len +  2 + match_cost(len) - thelen;
                return;
            }
            gain = literal_len + 3 + literal_cost(literal_len) + match_cost(len) - thelen;
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
    TokenSearcher(const uint8_t* begin, const uint8_t* end, Put_function put) : put(put), idx(begin), ip(begin), emitp(begin), data_begin(begin), data_end(end), hashtabele(HASH_SIZE,CHAIN_BREAK), chain(CHAIN_SIZE,ChainItem{})   {}


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
            put(NO_MATCH_OFS, ENCODE_MIN, emitp, ip - emitp);
            emitp = ip;
        }
    }

    void compressor() {

        ip+=ENCODE_MIN;
        if (ip > data_end) {
            ip = data_end;
        };

        for (;;)
        {
            if (ip + ENCODE_MIN > data_end){
                emit();
                break;
            }
            auto match = search_best(ip);
            if (match.len < ENCODE_MIN) {
                ip++;
                continue;
            }
            emit(match);
        }

    }
    Best search_best( const uint8_t* ip, const uint8_t* fast_skip=nullptr, bool first_short_match=false) {
        Best best{};
        auto chain_breaker = hash_of(ip);
        auto prev_pos = data_end;
        index(ip);

        for (int id = hashtabele[chain_breaker], false_sequence_count=0; false_sequence_count < MAX_FALSE_SEQUENCE_COUNT &&  id != CHAIN_BREAK; id = chain[id].next, false_sequence_count++)
        {
            const auto &item = chain[id];
            auto it = item.pos;
            assert(it != nullptr);

            if (item.hash != chain_breaker) {
                break;
            }

            if (it >= prev_pos) {
                //std::cerr << "break by pos!" << std::endl;
                break;
            }
            prev_pos = it;

            if (std::distance(it,ip) > MAX_OFFSET+32) {
                break;
            }

            //pre-hashing happens...
            //fast skip
            if (it+ENCODE_MIN > ip) {
                false_sequence_count = 0;
                continue;
            }
            //fast skip
            if (fast_skip && it > fast_skip) {
                false_sequence_count = 0;
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
            assert(ip2 >= emitp);
            assert(it + match <= ip2);

            Best after{it, ip2, match };
            after.optimize(*this);

            if (after.gain < best.gain) {
                best = after;
                if ( first_short_match && best.test_ofs() < (1<<10) ) {
                    break;
                }
                fast_skip = best.ofs;
                false_sequence_count >>= 2;

            }
        }
    return best;
    }
};


inline void lz_comp(const uint8_t*  begin, const uint8_t*  end, Put_function put) {
    auto current = begin;
    int plain_len = 0;
    auto ts = TokenSearcher(begin, end,put);
    ts.compressor();
}


#endif // LZ7_H
