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

/* expand operators */
using Put_bytes = std::function<void(uint32_t fetch, int n)>;
using Move_bytes = std::function<void(int offset , int len)>;
using Copy_bytes = std::function<void(const uint8_t* src, int len)>;
using Fill_pattern = std::function<void(int pattern_size, uint32_t pattern,int count)>;

void lz7expand(const uint8_t*  begin, const uint8_t*  end, Put_bytes put, Move_bytes move, Copy_bytes copy, Fill_pattern fill_pattern) {
    std::vector<uint8_t> out;
    out.reserve(1 << 20);
    auto in = begin;
    uint32_t value32;
    int bytes_available=0;

    auto fetch = [&]() {
        in -= bytes_available;
        if ( end - in >= 4) {
            bytes_available = 4;
            value32 = reinterpret_cast<const uint32_t*>(in)[0];
            in += 4;
            return;
        }
        value32 = 0;
        bytes_available = end - in;
        switch (bytes_available) {
            case 1:  value32 = in[0]; break;
            case 2:  value32 = in[0] | (in[1] << 8); break;
            case 3:  value32 = in[0] | (in[1] << 8) | (in[2] << 16); break;
        }
        in += bytes_available;
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
            uint32_t litval{0};
            if (literals_len > 0) {
                litval = value32 & ( (1 << (literals_len * 8)) - 1);
                skip(literals_len);
            }
            ofs |= get8_safe();

            if (match_len == 7) {
                for(;;) {
                    const auto chunk = get8_safe();
                    match_len += chunk;
                    if (chunk != 255) break;
                }
            }
            if (ofs == 0 ) {
                //offset == 0:
                //   LL == 0:  literal match_len count
                //   LL == 1:  rle x * match_len
                //   LL == 2:  rle xy * match_len
                //   LL == 3:  rle xyz * match_len
                switch (literals_len) {
                    case 0:
                        if (bytes_available >= match_len) {
                            put(value32, match_len);
                            skip(match_len);
                        } else {
                            in -= bytes_available; bytes_available = 0;
                            copy(in, match_len  );
                            in += match_len;
                        }
                        break;
                    case 1:
                    case 2:
                    case 3:
                        fill_pattern(literals_len, litval, match_len);
                        break;
                }
            }
            else {
                if (literals_len > 0)
                    put(litval, literals_len);
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

#if 1
    const char* filename = argv[1];
#else
    const char* filename = "D:\\Sources\\C++\\Projects\\lz7\\samples\\1.txt.lz7";
#endif
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
    size_t position{0};

    lz7expand(mmap.data(), mmap.data()+mmap.size(),
          //put function
          [&](uint32_t value, int literals_len) {
            //short literals
            if (literals_len) {
                std::cerr << std::setw(6) << std::setfill('0') << position <<  ": L<" << std::string_view(reinterpret_cast<const char*>(&value), literals_len) << ">" << std::endl;
                position += literals_len;
            }

        },
        //move function
        [&](int ofs, int len) {
            std::cerr << std::setw(6) << std::setfill('0') << position <<  ": M<" << ofs << "," << len << ">" << std::endl;
            position += len;

        },
        //copy function
        [&](const uint8_t* src, int len) {
            //std::cout << "L: " << std::string_view(reinterpret_cast<const char*>(src), len) << std::endl;
            std::cerr << std::setw(6) << std::setfill('0') << position <<  ": L<" << std::string_view(reinterpret_cast<const char*>(src), len) << ">" << std::endl;
            position += len;
        },
        //pattern dup function
        [&](int pattern_size, uint32_t pattern,int count) {
            std::cerr << std::setw(6) << std::setfill('0') << position <<  ": P<" << std::string_view(reinterpret_cast<const char*>(&pattern), pattern_size) << "> x " << count << std::endl;
            position += pattern_size * count;
        }
    );

     return 0;
}