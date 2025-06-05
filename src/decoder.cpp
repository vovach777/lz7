#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <cassert>
#include <cstdint>
#include <functional>
#include "mio.hpp"
#include "profiling.hpp"
#include "lz7.hpp"

using Put_bytes = std::function<void(uint32_t fetch, int n)>;
using Move_bytes = std::function<void(int offset , int len)>;
using Copy_bytes = std::function<void(const uint8_t* src, int len)>;

void lz7expand(const uint8_t*  begin, const uint8_t*  end, Put_bytes put, Move_bytes move, Copy_bytes copy) {
    std::vector<uint8_t> out;
    out.reserve(1 << 20);
    size_t outpos{0};
    auto in = begin;
    uint32_t value32;
    int bytes_available=0;

    auto fetch = [&]() {
        in -= bytes_available;
        bytes_available = 4;
        if ( end - in >= 4) {
            value32 = reinterpret_cast<const uint32_t*>(in)[0];
            in += 4;
            return;
        }
        value32 = 0;
        while (end - end > 0) {
            value32 <<= 8;
            value32 |= *in++;
        }
    };
    auto get8_safe = [&]() -> uint8_t {
        if (bytes_available == 0) {
            fetch();
        }
        uint8_t r = value32 & 0xFF;
        value32 >>= 8;
        bytes_available--;
        return r;
    };
    auto skip = [&](int count) {
        bytes_available -= count;
        value32 >>= count * 8;
    };


    for (;; ){
        fetch();
        if (value32 == 0) {
            return;
        }
        auto token = get8_safe();

        if (token & 0x80) {
            //short
            //1_xx_LL_MMM xxxx_xxxx
            int match_len = token & 0x7;
            int literals_len = (token >> 3) & 0x3;
            int ofs = ((token << 3) & 0x300);
            put(value32, literals_len);
            skip(literals_len);
            ofs |= get8_safe();

            if (match_len == 7) {
                for(;;) {
                    const auto chunk = get8_safe();
                    match_len += chunk;
                    if (chunk != 255) break;
                }
            }
            if (ofs == 0 ) {
                //literal only: extra literals = match
                if (match_len > 0) {
                    in -= bytes_available; bytes_available = 0;
                    copy(in, match_len  );
                }
            }
            else {
                match_len += ENCODE_MIN;
                move(ofs, match_len );
            }

        } else {
            //0_x_LLL_MMM xxxx_xxxx xxxx_xxxx
            int literals_len = (token >> 3) & 0x7;
            int match_len = token & 0x7;
            if (literals_len == 7) {
                for(;;) {
                    const auto chunk = get8_safe();
                    literals_len += chunk;
                    if (chunk != 255) break;
                }
            }
            if (bytes_available >= literals_len) {
                put(value32, literals_len);
                skip(literals_len);
            } else {
                in -= bytes_available; bytes_available = 0;
                copy(in, literals_len);
                in += literals_len;
            }
            int ofs = get8_safe();
            ofs |= get8_safe() << 8;
            ofs |= (token <<  (-6+16)) & 0x10000;
            if (match_len == 7) {
                for(;;) {
                    const auto chunk = get8_safe();
                    match_len += chunk;
                    if (chunk != 255) break;
                }
            }
            match_len += ENCODE_MIN;
            move(ofs, match_len );

        }

    }

}

int main(int argc, char** argv) {
    if (argc < 2) {
         std::cerr << "Usage: " << argv[0] << " <image_path>" << std::endl;
         return 1;
    }


    const char* filename = argv[1];
    // std::ifstream instream(filename, std::ios::in | std::ios::binary);
    // std::vector<uint8_t> data((std::istreambuf_iterator<char>(instream)), std::istreambuf_iterator<char>());
    std::error_code error;
    auto mmap = mio::make_mmap<mio::ummap_source>(filename, 0, 0, error);

    if (error)
    {
        std::cerr << error.message() << std::endl;
        return 1;
    }

    // std::ofstream outstream(std::string(filename) + ".lz7", std::ios::out | std::ios::binary);
    // if (!outstream) {
    //     std::cerr << "Failed to open output file" << std::endl;
    //     return 1;
    // }

    lz7expand(mmap.data(), mmap.data()+mmap.size(),
          //put function
          [&](uint32_t value, int literals_len) {
            //short literals
            std::cout << "L: " << std::string_view(reinterpret_cast<const char*>(&value), literals_len) << std::endl;
        },
        //move function
        [&](int ofs, int len) {
            std::cout << "M: " << ofs << " " << len << std::endl;
        },
        //copy function
        [&](const uint8_t* src, int len) {
            std::cout << "L: " << std::string_view(reinterpret_cast<const char*>(src), len) << std::endl;
        });

     return 0;
}