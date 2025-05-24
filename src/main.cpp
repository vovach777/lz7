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



    lz_comp(data.data(), data.data()+data.size(),[&](int offset, int len, const uint8_t * literals, int literals_len ) {

            assert(offset >= 0);
            assert(len >= 0);
            assert(literals_len >= 0);

        
           

            out.push_back( (std::min(len-ENCODE_MIN,15))  | std::min(literals_len,15) << 4 );
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

            assert(offset < 65536);
            out.push_back( (offset >> 8)  );
            out.push_back( offset & 0xff );

            if (len-ENCODE_MIN-15 >= 0) {
                
                for (int ext255=0;;)
                {
                    auto chunk = std::min(len-ENCODE_MIN-15-ext255*255,255);
                    out.push_back( chunk );
                    if (chunk < 255) break;  
                    ext255++;
                }
            }
    
    
        
        
    
                
            std::cout << std::setw(8) << len << " " << std::setw(8) << offset;
            if (literals_len > 0) {
                std::cout << " | ";
            

                while (literals_len--) {
                    std::cout << ( (*literals >= 32 &&  *literals <= 126) ?  (char)*literals : '.' );
                    literals++;
                }
             }
             std::cout << std::endl;



     });
     std::cout << data.size() << " -> " << out.size() << std::fixed << std::setprecision(2) << " (" << (out.size() * 100. / data.size()) << "%)" << std::endl;
     
     std::ofstream outstream(std::string(filename) + ".lz7", std::ios::out | std::ios::binary);
     if (outstream)
        outstream.write((char*)out.data(), out.size());
     return 0;
 }