#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <cassert>
#include <cstdint>
#include <functional>
#define ENCODE_MIN 4

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



    const uint8_t* in = data.data();
    int avail = data.size();

        uint32_t storage = reinterpret_cast<const uint32_t*>(in)[0];
        int bit_size = 32;

        auto getbits = [&](unsigned bits) {
            auto res = storage >> (32 - bits);
            storage <<= bits;
            bit_size -= bits;
            return res;
        };

        switch ( getbits(3) ) {
            case 0: //literal
                int len = getbits(5);
                in++;
                avail--;
                while (len--) {
                    out.push_back(*in++);
                    avail--;
                }
                in += 1;
                break;
            case 1: // offset:5, len=ENCODE_MIN
                auto dst_pos = out.size();
                out.resize(out.size() + ENCODE_MIN);
                const uint8_t* src = out.data() +  out.size() - ENCODE_MIN - getbits(5);
                std::memcpy(out.data() + dst_pos, src, 4);
                in += 1;
                break;
            case 2: //offset:13, len=ENCODE_MIN
                dst_pos = out.size();
                out.resize(out.size() + ENCODE_MIN);
                src = out.data() +  out.size() - ENCODE_MIN - getbits(13);
                std::memcpy(out.data() + dst_pos, src, 4);
                in += 2;
                break;
            case 3: //offset:16, len=ENCODE_MIN
                dst_pos = out.size();
                out.resize(out.size() + ENCODE_MIN);
                src = out.data() +  out.size() - ENCODE_MIN - getbits(16);
                std::memcpy(out.data() + dst_pos, src, 4);
                break;
            default:

        }

        if (!bypass) {
            offset -= ENCODE_MIN-1;
            assert(len >= ENCODE_MIN);
            len -= ENCODE_MIN;
        }
        if (bypass) {
            if (len == 0) {
                out.push_back(0);
                return; //EOF Marker
            }
            while (len > 0) {
                putbits(0, 3);//bypass marker
                int chunk = std::min(31,len);
                len -= chunk;
                putbits(chunk , 5);
                flush();
                while (chunk--)
                    out.push_back(*bypass++);
            }
        }
        else {
            if (len == 0) {
                if (offset < 32) {
                    putbits(1, 3);
                    putbits(offset, 5);
                } else
                if (offset < (1<<(5+8)) ) {
                    putbits(2, 3);
                    putbits(offset, 5+8);
                } else {
                    assert(offset < (1 << 16));
                    putbits(3, 3);
                    putbits(offset, 16);
                }
            } else {
                assert(len < 8192);
                if (offset < 1024 && len < 8) {
                    putbits(4, 3);
                    putbits(offset, 10);
                    putbits(len, 3);
                } else
                if (offset < 2048 && len < 4){
                    putbits(5, 3);
                    putbits(offset, 11);
                    putbits(len, 2);
                } else
                if ( len < 32){
                    putbits(6, 3);
                    putbits(offset, 16);
                    putbits(len, 5);
                } else {
                    putbits(7, 3);
                    putbits(offset, 16);
                    putbits(len, 5+8);
                }
                flush();
            }
        }



     });
     std::cout << data.size() << " -> " << out.size() << std::fixed << std::setprecision(2) << " (" << (out.size() * 100. / data.size()) << "%)" << std::endl;
     std::ofstream outstream(std::string(filename) + "-decoded", std::ios::out | std::ios::binary);
     if (outstream)
        outstream.write((char*)out.data(), out.size());
     return 0;
 }