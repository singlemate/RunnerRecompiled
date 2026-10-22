#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
// Diagnostic readback transport. Loopback only, opt-in, no guest input channel.
class MeGuestPreview {
 int listener_=-1,client_=-1;
 bool send_all(const void* ptr,size_t size){
  const auto* p=static_cast<const uint8_t*>(ptr);
  while(size){auto n=send(client_,p,size,MSG_NOSIGNAL);if(n<=0){if(n<0 && errno==EINTR)continue;close(client_);client_=-1;return false;}p+=n;size-=size_t(n);}return true;
 }
 public:
 MeGuestPreview(){
  if(!std::getenv("ME_GUEST_PREVIEW"))return;
  listener_=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener_<0)return;
  int yes=1;setsockopt(listener_,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
  sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(38761);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(bind(listener_,reinterpret_cast<sockaddr*>(&a),sizeof(a)) || listen(listener_,1)){close(listener_);listener_=-1;return;}
  fcntl(listener_,F_SETFL,O_NONBLOCK);
  std::printf("{\"event\":\"guest_preview_listening\",\"loopback_port\":38761}\n");
 }
 ~MeGuestPreview(){if(client_>=0)close(client_);if(listener_>=0)close(listener_);}
 bool connected(){
  if(client_<0 && listener_>=0){client_=accept4(listener_,nullptr,nullptr,SOCK_CLOEXEC);if(client_>=0){timeval t{0,500000};setsockopt(client_,SOL_SOCKET,SO_SNDTIMEO,&t,sizeof(t));}}
  return client_>=0;
 }
 void frame(const rex::ui::RawImage& f){
  if(client_<0)return;
  uint32_t header[]={htonl(0x4D454631),htonl(f.width),htonl(f.height),htonl(f.width*f.height*4)};
  if(!send_all(header,sizeof(header)))return;
  for(unsigned y=0;y<f.height;++y)if(!send_all(f.data.data()+size_t(y)*f.stride,size_t(f.width)*4))return;
 }
};
