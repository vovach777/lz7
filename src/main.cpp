#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <cassert>
#include <cstdint>
#include <functional>
#include "lz7.hpp"


int main(int argc, char** argv) {
    if (argc < 2) {
         std::cerr << "Usage: " << argv[0] << " <image_path>" << std::endl;
         return 1;
    }
    const char* filename = argv[1];
    std::ifstream instream(filename, std::ios::in | std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(instream)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> out;
    out.reserve(data.size() / 2);

    //std::cout << "len len_lg2 offset offset_lg2 literals literals_lg2" << std::endl;
    int super_short = 0;

    lz_comp(data.data(), data.data()+data.size(),[&](int offset, int len, const uint8_t * literals, int literals_len ) {

            assert(offset >= 0);
            assert(len >= 0);
            assert(literals_len >= 0);

            std::cout << std::setw(8) << len << " " << std::setw(8) << offset;
            if (len == 2) {
                super_short++;
            }
            if (literals_len > 0) {
                std::cout << " | ";
                if (literals_len > 15) {
                    out.push_back('!');
                }
                if (literals_len > 255) {
                    out.push_back('!');
                }

                while (literals_len--) {
                    out.push_back(*literals);
                    std::cout << ( (*literals >= 32 &&  *literals <= 126) ?  (char)*literals : '.' );
                    literals++;
                }
             }
             std::cout << std::endl;

            if (len==2) {
                out.push_back('s');
            } else
            if (len>8) {
                out.push_back('e');
            }
            if (len <= 8) {
                out.push_back('o');
            } else {
                out.push_back('O');
                out.push_back('O');
            }


            // std::cout << len << " " <<lz7::ilog2_32(len,-1)+1
            //           << " " << offset << " " <<  lz7::ilog2_32(offset,-1)+1
            //           << " " << literals_len << " " <<  lz7::ilog2_32(literals_len,-1)+1 << std::endl;


            // if (lz7::ilog2_32(literals_len,-1)+1 == 9) {
            //     std::cout << std::string_view((char*)literals, literals_len) << std::endl;
            // }



     });
     std::cout << data.size() << " -> " << out.size() << std::fixed << std::setprecision(2) << " (" << (out.size() * 100. / data.size()) << "%)" << std::endl;
     std::cout << super_short << " super shorts!" << std::endl;
     std::ofstream outstream(std::string(filename) + ".lz7", std::ios::out | std::ios::binary);
     if (outstream)
        outstream.write((char*)out.data(), out.size());
     return 0;
 }