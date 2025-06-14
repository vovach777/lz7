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

int main(int argc, char** argv) {
    if (argc < 2) {
         std::cerr << "Usage: " << argv[0] << " <filename>" << std::endl;
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
    size_t position{0};

    lz7::expand(mmap.data(), mmap.data()+mmap.size(),
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