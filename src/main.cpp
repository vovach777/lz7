#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <cassert>
#include <cstdint>
#include <functional>
#include "lz7.hpp"
#include "mio.hpp"
#include "profiling.hpp"
//#define DEBUG_OUTPUT


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

    std::vector<uint8_t> out;
    out.reserve(mmap.size() / 2);


    profiling::StopWatch sw;
    std::cout << "loading file into memory..." << std::flush;
    sw.start();
    std::vector<uint8_t> data(mmap.begin(), mmap.end());
    sw.stop();
    std::cout << " " << sw.elapsed_str() <<    std::endl;
    sw.startnew();
#ifdef DEBUG_OUTPUT
    size_t position{0};
#endif
    lz_comp(data.data(), data.data()+data.size(),
            [&](int offset, int len, const uint8_t * literals, int literals_len ) {
#ifdef DEBUG_OUTPUT
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
#endif
            //Token,[extlit],literals,offset,[ext-match]
            if (offset == 0) {
                //Token,literals,0
                assert(len == 0);
                out.push_back(0x80  | std::min(7,literals_len));//make sure if offset is 0 then match = literals len
                out.push_back(0); //offset low=0
                //ext-match = literals_len (offset == 0)
                if (literals_len-7 >= 0) {
                    for (int ext255=0;;)
                    {
                        auto chunk = std::min(literals_len-7-ext255*255,255);
                        out.push_back( chunk );
                        if (chunk < 255) break;
                        ext255++;
                    }
                }
                for (int i = 3; i < literals_len; ++i) {
                    out.push_back(literals[i]);
                }
                return;
            }
            assert(offset > 0);
            assert(len >= ENCODE_MIN);
            assert(literals_len >= 0);



            //out.push_back( (std::min(len-ENCODE_MIN,15))  | std::min(literals_len,15) << 4 );
            //1_xx_LL_MMM xxxx xxxx [extMMM]
            bool offset_10 = false;
            if (offset < (1 << 10) && literals_len < 4 ) {
                offset_10 = true;
                out.push_back( 0x80 | ((offset >> 8) << 5)  | (literals_len << 3) | std::min(7,len-ENCODE_MIN) );
            } else {
                assert(offset < (1 << 17));
                //0_x_LLL_MMM [extLLL]  xxxx_xxxx xxxx_xxxx [extMMM]
                out.push_back(  ((offset >> 16) << 6)  | (std::min(7,literals_len) << 3) | std::min(7,len-ENCODE_MIN)  );
            }

            if (literals_len-7 >= 0) {
                //offset_10 not pass here
                for (int ext255=0;;)
                {
                    auto chunk = std::min(literals_len-7-ext255,255);
                    out.push_back( chunk );
                    if (chunk < 255) break;
                    ext255+=255;
                }
            }
            for (int i = 0; i < literals_len; ++i) {
                out.push_back(literals[i]);
            }

            //assert(offset < 65536);
            out.push_back( offset & 0xff );
            if (!offset_10) {
                out.push_back( (offset >> 8) & 0xff );
            }

            if (len-ENCODE_MIN-7 >= 0) {

                for (int ext255=0;;)
                {
                    auto chunk = std::min(len-ENCODE_MIN-7-ext255,255);
                    out.push_back( chunk );
                    if (chunk < 255) break;
                    ext255+=255;
                }
            }

     });
     sw.stop();

     std::cout << mmap.size() << " -> " << out.size() << std::fixed << std::setprecision(2) << " (" << (out.size() * 100. / data.size()) << "%)" << std::endl;
     std::cout << "time: " << sw.elapsed_str() << std::endl;
     std::cout << "speed: " << std::fixed << std::setprecision(2) <<  data.size() / sw.elapsed() / 1024 / 1024 << " MB/s" << std::endl;

     std::ofstream outstream(std::string(filename) + ".lz7", std::ios::out | std::ios::binary);
     if (outstream)
        outstream.write((char*)out.data(), out.size());
     return 0;
 }