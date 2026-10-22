#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
struct MeOfflineNetwork {
    int fd=-1;uint16_t port=0;std::array<uint8_t,13> params{13};
    ~MeOfflineNetwork(){close();}
    void close(){if(fd>=0)::close(fd);fd=-1;port=0;}
    bool start(const uint8_t* config=nullptr){
        if(fd<0){
            const int candidate=::socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
            if(candidate<0)return false;
            sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if(::bind(candidate,reinterpret_cast<sockaddr*>(&address),sizeof(address))){::close(candidate);return false;}
            socklen_t size=sizeof(address);
            if(::getsockname(candidate,reinterpret_cast<sockaddr*>(&address),&size) || address.sin_addr.s_addr!=htonl(INADDR_LOOPBACK) || !address.sin_port){::close(candidate);return false;}
            fd=candidate;port=ntohs(address.sin_port);
        }
        if(config)std::memcpy(params.data(),config,params.size());
        return true;
    }
};
