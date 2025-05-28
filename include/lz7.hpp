#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define MAX_OFFSET ((1 << 17)-1)
#define NO_MATCH_OFS (MAX_OFFSET)
#define ENCODE_MIN (4)
#define CHAIN_DISTANCE (64)
#define MAX_FALSE_SEQUENCE_COUNT (8)
#define GIVE_A_CHANCE_COUNT (0)
//#define MAX_FALSE_MATCH_COUNT (8)
#define MAX_LEN (65535)
#define MAX_MATCH (MAX_LEN+ENCODE_MIN)
#define HASH_LOG2 (22)
#define CHAIN_LOG2 (14)
#if CHAIN_LOG2 > 16
    #error "CHAIN_LOG2 > 16 !!!"
#endif
#define CHAIN_SIZE (1 << CHAIN_LOG2)
#define CHAIN_BREAK (CHAIN_SIZE-1)
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
                        register_chain_item(idx);
                        if (current_idx_rle >= 8)
                            std::cout << "RLE: " << std::string_view((const char*)idx-current_idx_rle, std::min(16, current_idx_rle) ) << "/" <<  current_idx_rle << std::endl;
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
        int gain{0x8000};
        bool is_self_reference( const TokenSearcher& ctx ) {
            return ofs + len > ctx.emitp;
        }
        auto test_ofs() const {
            return std::distance( ofs+len , ip2);
        }
        // auto test_len() const {
        //     return std::max(0, len - ENCODE_MIN);
        // }
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
                gain = literal_len +  3 + match_cost(len) - len;
                return;
            }
            gain = literal_len + 4 + literal_cost(literal_len) + match_cost(len) - len;
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

        bool fast_mode = true;
        Best lazy_best{};

        //std::cerr <<  std::string_view((const char*) ip,std::min<ptrdiff_t>(8, data_end-ip)) << std::endl;
        for (;;)
        {
            if (ip + ENCODE_MIN  >= data_end) {
                ip=data_end;
                emit();
                break;
            }
            index(ip);
            auto greedy_best_match = search_best(ip, {}, ENCODE_MIN, false);
            if ( greedy_best_match.len < ENCODE_MIN) {
                ip += 3;
                continue;
            }
            if ( greedy_best_match.is_self_reference(*this)) {
                emit(greedy_best_match);
                continue;
            }
            lazy_best = greedy_best_match;
            int  m = 0;
            auto sp = std::max( ip+1,  lazy_best.ip2 + 1  );
            for (int n = 0; n < LOOK_AHEAD;++n)
            {
                auto lazy=search_best( sp, lazy_best, lazy_best.len, false);
                if (lazy.gain < lazy_best.gain) {
                    std::cerr << '.';
                    lazy_best = lazy;
                    ++m;
                    sp = std::max( sp+1,  lazy_best.ip2 + 1  );
                } else
                  sp += (n >> 1) + 1;

            }
            emit(lazy_best);
            if (m > 0)
                std::cerr << std::endl;



        }

    }
    Best search_best( const uint8_t* ip, Best initial_best,  int min_match=ENCODE_MIN, bool first_short_match=false) {
        Best best = initial_best;
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

            if (std::distance(it,ip) > MAX_OFFSET+32) {
                break;
            }

            //pre-hashing! ops! it`s happens...
            if ( (ip+ENCODE_MIN - it)  < min_match) {
                //std::cerr << emitp-it << " : fast rewind optimization!" << std::endl;
                false_sequence_count = 0;
                continue;
            }

            prev_pos = it;


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
                // else
                //     std::cerr << "the literal absorption optimization!" << std::endl;
            }
            if (match < min_match) {
                continue;
            }
            assert(ip2 >= emitp);
            assert(it + match <= ip2);

            Best after{it, ip2, match };
            after.optimize(*this);

            if (after.gain < best.gain) {
                best = after;
                false_sequence_count-=GIVE_A_CHANCE_COUNT; //one more try...
                if ( first_short_match && best.test_ofs() < (1<<10) ) {
                    break;
                }
            } else {
                //false_match_count++;
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
