#ifndef LZ7_H
#define LZ7_H

#define HAS_BUILTIN_CLZ
#define MAX_OFFSET ((1 << 17)-1)
#define NO_MATCH_OFS (MAX_OFFSET)
#define ENCODE_MIN (4)
#define MAX_FALSE_SEQUENCE_COUNT (32)
#define MAX_LEN (65535)
#define MAX_MATCH (MAX_LEN+ENCODE_MIN)
#define HASH_LOG2 (16)
#define HASH_SIZE (1 << HASH_LOG2)
#define CHAIN_LOG2 (16)
#define CHAIN_SIZE (1 << CHAIN_LOG2)
#define CHAIN_BREAK (CHAIN_SIZE - 1)
#define LOOK_AHEAD (2)
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
        if (idx+ENCODE_MIN>=data_end) return;
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
        while (idx < ip) {
            if (idx[0] == idx[1]) {
                if (((current_idx_rle - 1) & current_idx_rle) == 0) {
                    if (current_idx_rle == 0 || current_idx_rle >= 8) {
                        register_chain_item(idx);
                        // if (current_idx_rle >= 8)
                        //     std::cout << "RLE: " << std::string_view((const char*)idx-current_idx_rle, std::min(16, current_idx_rle) ) << "/" <<  current_idx_rle << std::endl;
                    }
                }
                current_idx_rle++;
            } else {
                register_chain_item(idx);
                current_idx_rle = 0;
            }
            idx++;
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

            if (len < ENCODE_MIN || test_ofs() > MAX_OFFSET) {
                set_to_literals(ctx);
                return;
            }
            assert(test_ofs() != 0);
            auto literal_len = std::distance(ctx.emitp,ip2);
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

    uint16_t hash_of(const uint8_t* it) const {
        #if ENCODE_MIN == 3
        return static_cast<uint16_t>( (reinterpret_cast<const uint32_t*>(it)[0]&0xffffff) * 2654435761u >> (32-HASH_LOG2) );
        #else
        return static_cast<uint16_t>( reinterpret_cast<const uint32_t*>(it)[0] * 2654435761u >> (32-HASH_LOG2) );
        #endif
    }

    public:
    TokenSearcher(const uint8_t* begin, const uint8_t* end, Put_function put) : put(put), idx(begin), ip(begin), emitp(begin), data_begin(begin), data_end(end), hashtabele(HASH_SIZE,CHAIN_BREAK), chain(CHAIN_SIZE,ChainItem{}) {}


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
            put(0, 0, emitp, ip - emitp);
            emitp = ip;
        }
    }

    void compressor() {
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
            auto match = search_best(ip,nullptr,false);
            if (match.len < ENCODE_MIN) {
                ip++;
                continue;
            }
            ip += 1;

            for (int i = 1; i <= LOOK_AHEAD; ++i)
            {
                if (ip + ENCODE_MIN > data_end) break;
                if (std::memcmp(ip-1, ip, ENCODE_MIN) != 0) {
                    auto next_match = search_best(ip,nullptr,false);
                    if (next_match.gain > match.gain) {
                        match = next_match;
                    }              
                }
                ip += 1;
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
            if (it >= ip) {
                false_sequence_count = 0;
                continue;
            }
            //fast skip
            if (fast_skip && it > fast_skip) {
                false_sequence_count = 0;
                continue;
            }

            auto [ it_mismatch, ip_mismatch ] = std::mismatch(it, data_end, ip, data_end);
            int match_len = std::distance(it, it_mismatch);
            if (match_len < ENCODE_MIN) {
                continue;
            }
            // if (it_mismatch > ip) {
            //   //  std::cerr << "self-ref: " <<  it_mismatch-ip <<   std::endl;
            // }
            //back matching
            auto ip2 = ip;

            while (  it > data_begin  &&  ip2 > emitp  &&  it[-1] == ip2[-1] ) {
                --it;
                --ip2;
                ++match_len;
            }
            assert(ip2 >= emitp);
            //assert(it + match <= ip2);

            Best after{it, ip2, match_len };
            after.optimize(*this);

            if (after.gain > best.gain) {
                best = after;
                //std::cerr << " gain search step: " << (best.gain - after.gain) << std::endl;
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
