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

          
            if (len == ENCODE_MIN) {
                assert(offset <= 127);
                assert(literals_len == 0);
                out.push_back(0x80 | offset );
                super_short++;
                
            } else {
            //pack length
           
            //0xyy zzzz
            //yy (min=2) 2,3,4,ext8
            out.push_back((offset > 255 ? 0x40 : 0) && (std::min(len-ENCODE_MIN,3)) << 4 | std::min(literals_len,15));
            if (literals_len-15 >= 0) {
                
                for (int ext255=0;;)
                {
                    auto chunk = std::min(literals_len-15-ext255*255,255);
                    out.push_back( chunk );
                    if (chunk < 255) break;  
                    ext255++;
                }
            }
            for (int i = 0; i < literals_len; ++i) {
                out.push_back(literals[i]);
            }
            if (len-ENCODE_MIN-3 >= 0) {
                
                for (int ext255=0;;)
                {
                    auto chunk = std::min(len-ENCODE_MIN-3-ext255*255,255);
                    out.push_back( chunk );
                    if (chunk < 255) break;  
                    ext255++;
                }
            }
            out.push_back(offset & 0xff);
            if (len > 255) {
                assert(offset <= 65535);
                out.push_back(offset >> 8);
            }
            }
                
            std::cout << std::setw(8) << len << " " << std::setw(8) << offset;
            if (literals_len > 0) {
                std::cout << " | ";
            

                while (literals_len--) {
                    out.push_back(*literals);
                    std::cout << ( (*literals >= 32 &&  *literals <= 126) ?  (char)*literals : '.' );
                    literals++;
                }
             }
             std::cout << std::endl;



     });
     std::cout << data.size() << " -> " << out.size() << std::fixed << std::setprecision(2) << " (" << (out.size() * 100. / data.size()) << "%)" << std::endl;
     std::cout << super_short << " super shorts!" << std::endl;
     std::ofstream outstream(std::string(filename) + ".lz7", std::ios::out | std::ios::binary);
     if (outstream)
        outstream.write((char*)out.data(), out.size());
     return 0;
 }