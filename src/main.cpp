#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <filesystem>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#define _chsize_s(d, s) ftruncate(d,s)
#endif
namespace fs = std::filesystem;
#include "lz7.hpp"
#include "mio.hpp"
#include "profiling.hpp"
#include "myargs.hpp"


inline void createEmptyFile(const char * fileName, int64_t fileSize) {
    FILE *fp=fopen(fileName, "w");
    _chsize_s(fileno(fp),fileSize);
    fclose(fp);
}

inline void extendFile(const char * fileName, int64_t fileSize) {
    FILE *fp=fopen(fileName, "r+");
    _chsize_s(fileno(fp),fileSize);
    fclose(fp);
}

using myargs::args;
int main(int argc, char** argv) {
    args.parse(argc, argv);
    if (args.size() < 2) {
         std::cerr << "Usage: " << args[0] << " <filename>" << std::endl;
         return 1;
    }
    auto is_decoder = args.has('d') || args.has("decode");
    auto filename = args.size() >= 3 ? args[2] : args[1] + ( is_decoder ? ".orig" : ".lz7" );
    // std::ifstream instream(filename, std::ios::in | std::ios::binary);
    // std::vector<uint8_t> data((std::istreambuf_iterator<char>(instream)), std::istreambuf_iterator<char>());
    std::error_code error;
    auto mmap = mio::make_mmap<mio::ummap_source>(args[1], 0, 0, error);

    if (error)
    {
        std::cerr << error.message() << std::endl;
        return 1;
    }

    profiling::StopWatch sw;
    auto is_benchmark = args.has('b') || args.has("benchmark");
    if (!is_decoder) {
        std::vector<uint8_t> out;
        out.reserve(mmap.size() / 2);
        auto bi = std::back_inserter(out);

        auto lz7_fmt_1 =
                [&](int offset, int len, const uint8_t * literals, int literals_len ) {
                //Token,[extlit],literals,offset,[ext-match]
                if (offset == 0) {
                    //Token,literals,0
                    assert(len == 0);
                    *bi = (0x80  | std::min(7,literals_len));//make sure if offset is 0 then match = literals len
                    *bi = (0); //offset low=0
                    //ext-match = literals_len (offset == 0)
                    if (literals_len-7 >= 0) {
                        for (int ext255=0;;)
                        {
                            auto chunk = std::min(literals_len-7-ext255*255,255);
                            *bi = chunk;
                            if (chunk < 255) break;
                            ext255++;
                        }
                    }
                    std::copy(literals, literals+literals_len, bi);
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
                    *bi = ( 0x80 | ((offset >> 8) << 5)  | (literals_len << 3) | std::min(7,len-ENCODE_MIN) );
                } else {
                    assert(offset < (1 << 17));
                    //0_x_LLL_MMM [extLLL]  xxxx_xxxx xxxx_xxxx [extMMM]
                    *bi = (  ((offset >> 16) << 6)  | (std::min(7,literals_len) << 3) | std::min(7,len-ENCODE_MIN)  );
                }

                if (literals_len-7 >= 0) {
                    //offset_10 not pass here
                    for (int ext255=0;;)
                    {
                        auto chunk = std::min(literals_len-7-ext255,255);
                        *bi = ( chunk );
                        if (chunk < 255) break;
                        ext255+=255;
                    }
                }
                std::copy(literals, literals+literals_len, bi);

                //assert(offset < 65536);
                *bi = ( offset & 0xff );
                if (!offset_10) {
                    *bi = ( (offset >> 8) & 0xff );
                }

                if (len-ENCODE_MIN-7 >= 0) {

                    for (int ext255=0;;)
                    {
                        auto chunk = std::min(len-ENCODE_MIN-7-ext255,255);
                        *bi = ( chunk );
                        if (chunk < 255) break;
                        ext255+=255;
                    }
                }

        };

        if (is_benchmark) {
            std::cout << "loading file into memory..." << std::flush;
            sw.start();
            std::vector<uint8_t> data(mmap.begin(), mmap.end());
            sw.stop();
            std::cout << " " << sw.elapsed_str() <<    std::endl;
            sw.startnew();
            lz7::compress(data.data(), data.data()+data.size(), lz7_fmt_1);
            sw.stop();
        } else {
            sw.startnew();
            lz7::compress(mmap.data(), mmap.data()+mmap.size(), lz7_fmt_1);
            sw.stop();
        }



        std::cout << mmap.size() << " -> " << out.size() << std::fixed << std::setprecision(2) << " (" << (out.size() * 100. / mmap.size()) << "%)" << std::endl;
        std::cout << "time: " << sw.elapsed_str() << std::endl;
        std::cout << "speed: " << std::fixed << std::setprecision(2) <<  mmap.size() / sw.elapsed() / 1024 / 1024 << " MB/s" << std::endl;

        if (!is_benchmark) {
            std::ofstream outstream(std::string(filename), std::ios::out | std::ios::binary);
            if (outstream)
                outstream.write((char*)out.data(), out.size());
        }
    } else {
        constexpr int PgSize = (1 << 20);
        //static_assert( PgSize > MAX_OFFSET );
        std::cout << "decompressing " <<  args[1]    << " into " << std::string(filename) << "..." << std::flush;
        int64_t base = 0;
        int64_t current_size = PgSize;
        createEmptyFile(filename.data(), PgSize);

        auto rw_mmap = mio::make_mmap<mio::ummap_sink>(filename, base, current_size, error);
        if (error)
        {
            throw std::runtime_error(error.message());
        }
        auto it = rw_mmap.begin();
        auto end =  rw_mmap.end();
        auto chk_expand = [&](int size) {
            if (it + size > end)  {
                auto global_it = it - rw_mmap.begin() + base;
                auto global_window = std::max<int64_t>(0, global_it - MAX_OFFSET);
                auto it_distance = global_it - global_window;
                current_size = (it_distance + size + PgSize + PgSize -1) & ~(PgSize - 1);
                base = global_window;
                extendFile(filename.data(), base + current_size);
                rw_mmap = mio::make_mmap<mio::ummap_sink>(filename, base, current_size, error);
                if (error)
                {
                    throw std::runtime_error(error.message());
                }
                it = rw_mmap.begin() + it_distance;
                end = rw_mmap.end();
                assert(it < end);
            }
        };


        lz7::expand(mmap.data(), mmap.data()+mmap.size(),
          //put function
          [&](uint32_t value, int literals_len) {
            //short literals
            if (literals_len) {
                chk_expand(4);
                reinterpret_cast<uint32_t*>(it)[0] = value;
                it += literals_len;
            }
        },
        //move function
        [&](int ofs, int len) {
            chk_expand(len);
            //std::memcpy(it, it - ofs, len);
            for (int i = 0; i < len; ++i) {
                it[i] = it[i - ofs];
            }
            it += len;
        },
        //copy function
        [&](const uint8_t* src, int len) {
            chk_expand(len);
            std::memcpy(it, src, len);
            it += len;
        },
        //pattern dup function
        [&](int pattern_size, uint32_t pattern,int count) {
            chk_expand(pattern_size * count);
            reinterpret_cast<uint32_t*>(it)[0] = pattern;
            std::memmove(it + pattern_size, it, (count-1)*pattern_size);
            it += count*pattern_size;
        });
        auto pos = it - rw_mmap.begin();
        rw_mmap.unmap();
        base += pos;
        fs::resize_file(filename, base);
        std::cout << " done." << std::endl;
    }
 }