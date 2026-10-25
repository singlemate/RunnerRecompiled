#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <mutex>
#include <deque>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
struct MeInputSnapshot {uint32_t sequence=0;bool connected=false;uint16_t buttons=0;int16_t axes[4]{};uint8_t lt=0,rt=0;};
inline bool me_decode_input(const uint8_t* p,size_t n,MeInputSnapshot& out){
 if(n!=32 || p[0]!='M' || p[1]!='E' || p[2]!='I' || p[3]!='1')return false;
 auto u32=[&](unsigned i){return uint32_t(p[i])<<24|uint32_t(p[i+1])<<16|uint32_t(p[i+2])<<8|p[i+3];};
 if(u32(8)>1 || (u32(12)&0xffff0000u))return false;
 for(unsigned i=26;i<32;++i)if(p[i])return false;
 out.sequence=u32(4);out.connected=u32(8)!=0;out.buttons=uint16_t(u32(12));out.lt=p[24];out.rt=p[25];
 for(unsigned i=0;i<4;++i)out.axes[i]=int16_t(uint16_t(p[16+i*2])<<8|p[17+i*2]);
 return true;
}
struct MeLocalPlayer {uint64_t xuid=0;char name[16]{};};
inline bool me_decode_player(const uint8_t* p,size_t n,MeLocalPlayer& out){
 if(n!=32 || std::memcmp(p,"MEP1",4) || p[4] || p[5] || p[6] || p[7])return false;
 uint64_t id=0;for(unsigned i=8;i<16;++i)id=id<<8|p[i];
 if((id&0xffff000000000000ull)!=0xe000000000000000ull || !(id&0x0000ffffffffffffull))return false;
 bool ended=false;for(unsigned i=16;i<32;++i){if(!p[i])ended=true;else if(ended || p[i]<32 || p[i]>126)return false;}
 if(!ended || !p[16])return false;out.xuid=id;std::memcpy(out.name,p+16,16);return true;
}
class MeGuestInput {
 int fd_=-1;std::mutex mutex_;MeInputSnapshot state_{};std::chrono::steady_clock::time_point received_{};std::deque<MeInputSnapshot> changes_;MeLocalPlayer player_{};
 public:
 MeGuestInput(){if(!std::getenv("ME_GUEST_INPUT"))return;fd_=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd_<0)return;
 sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(38762);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(bind(fd_,reinterpret_cast<sockaddr*>(&a),sizeof(a))){close(fd_);fd_=-1;return;}fcntl(fd_,F_SETFL,O_NONBLOCK);
 std::puts("{\"event\":\"guest_input_listening\",\"port\":38762}");}
 ~MeGuestInput(){if(fd_>=0)close(fd_);}
 MeLocalPlayer player(){poll();std::lock_guard<std::mutex> lock(mutex_);return player_;}
 MeInputSnapshot poll(bool consume=false){std::lock_guard<std::mutex> lock(mutex_);if(fd_<0)return {};
 uint8_t packet[64];for(unsigned i=0;i<128;++i){const auto n=recv(fd_,packet,sizeof(packet),0);if(n<0)break;MeInputSnapshot next;
 MeLocalPlayer identity;
 if(me_decode_player(packet,size_t(n),identity)){if(!player_.xuid){player_=identity;std::puts("{\"event\":\"local_player_identity_received\"}");}continue;}
 if(!me_decode_input(packet,size_t(n),next))continue;if(next.buttons!=state_.buttons || next.connected!=state_.connected){if(changes_.size()>=32)changes_.pop_front();changes_.push_back(next);}
 state_=next;received_=std::chrono::steady_clock::now();}
 // Unconsumed press/release changes survive a slow guest poll (menus can run at
 // ~0.5 fps here) for up to 5 s; the held state itself still expires after 600 ms.
 const auto age=std::chrono::steady_clock::now()-received_;
 if(age>std::chrono::seconds(5))changes_.clear();
 if(consume && !changes_.empty()){auto value=changes_.front();changes_.pop_front();return value;}
 if(age>std::chrono::milliseconds(600))return {};
 return state_;}
};
inline MeGuestInput& me_guest_input(){static MeGuestInput input;return input;}
