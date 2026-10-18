#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <filesystem>
namespace fs = std::filesystem;
#include "lz7.hpp"
#include "mio.hpp"
#include "profiling.hpp"
#include "myargs.hpp"


inline void createEmptyFile(const char * fileName) {
    FILE *file = fopen(fileName, "wb");
    if (file)
        fclose(file);
}

inline void extendFile(const char * fileName, int64_t fileSize) {
    fs::resize_file(fileName, fileSize);
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
        constexpr int64_t PgSize = (1 << 20);
        std::cout << "decompressing " <<  args[1]    << " into " << std::string(filename) << "..." << std::flush;
        sw.startnew();
        // выход мапится окном [base, base + current_size); при нехватке места окно сдвигается,
        // сохраняя MAX_OFFSET байт истории, и растёт геометрически
        int64_t base = 0;
        int64_t current_size = PgSize;
        createEmptyFile(filename.data());
        extendFile(filename.data(), current_size);
        auto rw_mmap = mio::make_mmap<mio::ummap_sink>(filename, base, current_size, error);
        if (error)
        {
            throw std::runtime_error(error.message());
        }
        const uint8_t *in = mmap.data();
        const uint8_t *in_end = mmap.data() + mmap.size();
        uint8_t *op = rw_mmap.begin();
        for (;;)
        {
            auto r = lz7::decode(in, in_end, op, rw_mmap.end(), rw_mmap.begin());
            in = r.in;
            op = r.op;
            if (r.status == lz7::DecodeStatus::Done)
                break;
            if (r.status == lz7::DecodeStatus::Error)
            {
                std::cerr << std::endl << "format broken: " << r.error << " at input offset " << (in - mmap.data()) << std::endl;
                return 1;
            }
            const int64_t global_op = base + (op - rw_mmap.begin());
            const int64_t new_base = std::max<int64_t>(0, global_op - MAX_OFFSET);
            const int64_t history = global_op - new_base;
            const int64_t grow = std::max<int64_t>(r.need, std::clamp<int64_t>(global_op, PgSize, int64_t(1) << 30));
            current_size = (history + grow + PgSize - 1) & ~(PgSize - 1);
            base = new_base;
            rw_mmap.unmap();
            extendFile(filename.data(), base + current_size);
            rw_mmap = mio::make_mmap<mio::ummap_sink>(filename, base, current_size, error);
            if (error)
            {
                throw std::runtime_error(error.message());
            }
            op = rw_mmap.begin() + history;
        }
        auto pos = op - rw_mmap.begin();
        rw_mmap.unmap();
        base += pos;
        fs::resize_file(filename, base);
        sw.stop();
        std::cout << " done." << std::endl;
        std::cout << "time: " << sw.elapsed_str() << std::endl;
        std::cout << "speed: " << std::fixed << std::setprecision(2) << base / sw.elapsed() / 1024 / 1024 << " MB/s" << std::endl;
    }
 }