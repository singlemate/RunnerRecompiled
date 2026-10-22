#include <image.h>
#include <fstream>
#include <vector>
#include <iostream>
int main(int argc,char**argv){
 if(argc!=3)return 2;
 std::ifstream input(argv[1],std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(input)),{});
 if(b.size()<24)return 3;auto image=Image::ParseImage(b.data(),b.size());
 if(!image.data || !image.size)return 4;
 auto be32=[&](size_t p)->uint32_t {if(p+4>b.size())throw 1;return uint32_t(b[p])<<24|uint32_t(b[p+1])<<16|uint32_t(b[p+2])<<8|b[p+3];};
 size_t decoded=0;
 for(uint32_t i=0;i<be32(20);++i)if(be32(24+i*8)==0x3ff){
   size_t p=be32(28+i*8);uint32_t n=be32(p);
   if(n<8 || n%8 || p+n>b.size() || b[p+6]!=0 || b[p+7]!=1)return 6;
   for(size_t j=8;j<n;j+=8)decoded+=uint64_t(be32(p+j))+be32(p+j+4);
 }
 if(!decoded || decoded>image.size)return 7;
 std::ofstream out(argv[2],std::ios::binary);out.write((const char*)image.data.get(),decoded);
 out.flush();
 std::cout<<"base "<<std::hex<<image.base<<" size "<<image.size<<"\n";return out.good()?0:5;
}
