#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

// External TV font: 128 characters, 7 rows each, 5 low bits per row.
// No manual-derived character patterns are embedded in this source.
class LkitTvFont {
public:
    static constexpr size_t byteCount=128u*7u;
    using Bytes=std::array<uint8_t,byteCount>;
    Bytes bytes{};
    static unsigned glyphIndex(uint8_t code){
        return code>=0x20&&code<=0x5f?code-0x20:
            code>=0xa0&&code<=0xdf?code-0xa0+64:0;
    }
    static uint32_t unicode(unsigned glyph){
        if(glyph<64)return glyph==0x3c?0x00a5u:0x20u+glyph;
        return glyph==64?0x20u:0xff61u+glyph-65;
    }
    uint8_t row(unsigned glyph,unsigned y)const{
        return glyph<128&&y<7?bytes[glyph*7+y]:0;
    }
    static bool valid(const Bytes& b){for(uint8_t v:b)if(v&0xe0u)return false;return true;}
    bool load(const std::filesystem::path& path,std::string* err=nullptr){
        std::ifstream f(path,std::ios::binary);
        if(!f){if(err)*err="open";return false;}
        Bytes next{};f.read(reinterpret_cast<char*>(next.data()),std::streamsize(next.size()));
        if(f.gcount()!=std::streamsize(next.size())){if(err)*err="size";return false;}
        char extra;if(f.get(extra)){if(err)*err="size";return false;}
        if(!f.eof()||f.bad()){if(err)*err="read";return false;}
        if(!valid(next)){if(err)*err="bits";return false;}
        bytes=next;return true;
    }
    bool save(const std::filesystem::path& path,std::string* err=nullptr)const{
        if(!valid(bytes)){if(err)*err="bits";return false;}
        std::ofstream f(path,std::ios::binary);
        if(!f){if(err)*err="open";return false;}
        f.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
        f.close();if(!f){if(err)*err="write";return false;}return true;
    }
    // Area-average a top-down 32-bit grayscale GDI bitmap to the TV's 5x7 grid.
    // The threshold keeps thin strokes after shrinking; this is an approximation.
    static uint8_t sampleRow(const uint32_t* pixels,int stride,int width,int height,unsigned y){
        if(!pixels||stride<width||width<=0||height<=0||y>=7)return 0;
        uint8_t result=0;
        const int y0=int(y)*height/7,y1=int(y+1)*height/7;
        for(int x=0;x<5;x++){
            const int x0=x*width/5,x1=(x+1)*width/5;
            uint64_t sum=0,count=0;
            for(int py=y0;py<y1;py++)for(int px=x0;px<x1;px++){
                const uint32_t p=pixels[py*stride+px];
                sum+=((p&255u)+((p>>8)&255u)+((p>>16)&255u))/3u;++count;
            }
            if(count&&sum>=count*64u)result|=uint8_t(0x10u>>x);
        }
        return result;
    }
};
