#include <vector>
#include <iomanip>
#include <cassert>
#include <cstdint>
#include <functional>
#include "lz7.hpp"
#include "mio.hpp"



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

 
    std::vector<uint8_t> data(mmap.begin(), mmap.end());


    size_t position{0};

    lz7::compress(data.data(), data.data()+data.size(),
            [&](int offset, int len, const uint8_t * literals, int literals_len ) {

            if ( offset == 0) {
                assert(len == 0); //pattern not supported yet
                std::cerr << std::setw(6) << std::setfill('0') << position <<  ": L<" <<  std::string_view(reinterpret_cast<const char*>(literals), literals_len) << ">" << std::endl;
                position += literals_len;
            } else {
                if (literals_len) {
                    std::cerr << std::setw(6) << std::setfill('0') << position <<  ": L<" << std::string_view(reinterpret_cast<const char*>(literals), literals_len) << ">" << std::endl;
                    position += literals_len;
                }
                std::cerr << std::setw(6) << std::setfill('0') << position <<  ": M<" << offset << "," << len << ">" << std::endl;
                position += len;
            }


     }); 
     return 0;
 }