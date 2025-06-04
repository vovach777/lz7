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

void lz7expand(const uint8_t*  begin, const uint8_t*  end, Put_function put) {
    std::vector<uint8_t> out;
    out.reserve(1 << 20);
    size_t outpos{0};
    for (auto it = begin; it != end; ){
        uint8_t token = *it++;
        
        if (token & 0x80) {
            //short
            //1_xx_LL_MMM xxxx_xxxx
            int literals = token >> 3 & 0x3;
            int match_len = token & 0x7;
            if (it + literals > end) {
                std::cerr << "fail" << std::endl;
                return;
            }
            out.resize(outpos + literals);
            std::memcpy(&out[outpos], it, literals);
            outpos += literals;
            it += literals;
            if (it  >= end) {
                std::cerr << "fail" << std::endl;
                return;
            }
            int ofs = (*it++ | (token << (3+8))) & ((1<<10)-1);
            if (match_len == 7) {
                for(;;) {
                    if (it  >= end) {
                        std::cerr << "fail" << std::endl;
                        return;
                    }   
                    const auto chunk = *it++;
                    match_len += *it++;
                    if (chunk != 255) break;
                }          
            }
            match_len += ENCODE_MIN;
            out.resize(outpos + match_len);
            std::memmove(&out[outpos], &out[outpos - ofs], match_len);
            outpos += match_len;

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

    std::ofstream outstream(std::string(filename) + ".lz7", std::ios::out | std::ios::binary);
     if (!outstream) {
        std::cerr << "Failed to open output file" << std::endl;
        return 1;
     }
    #define flush(out) outstream.write((char*)out.data(), out.size()); 

    const uint8_t* in = mmap.data();
    auto avail = mmap.size();

  

     return 0;
 }