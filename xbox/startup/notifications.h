#pragma once
#include <cstdint>
#include <vector>
#include <deque>
#include <utility>
struct MeNotifications {
    struct Listener {uint64_t mask;uint32_t version;bool live=true;std::deque<std::pair<uint32_t,uint32_t>> queue;};
    std::vector<Listener> objects;
    Listener* find(uint32_t h){if(h<0x50000 || (h&3))return nullptr;auto i=(h-0x50000)/4;return i<objects.size()&&objects[i].live?&objects[i]:nullptr;}
    uint32_t create(uint64_t mask,uint32_t version){if(objects.size()>=256 || version>10)return 0;objects.push_back({mask,version,true,{}});return 0x50000+uint32_t(objects.size()-1)*4;}
    static bool accepts(const Listener& l,uint32_t id){return l.live && (l.mask&(uint64_t(1)<<((id>>25)&63))) && ((id>>16)&511)<=l.version;}
    int broadcast(uint32_t id,uint32_t data){for(const auto& l:objects)if(accepts(l,id)&&l.queue.size()>=1024)return -1;int n=0;for(auto& l:objects)if(accepts(l,id)){l.queue.push_back({id,data});++n;}return n;}
    bool pop(uint32_t h,uint32_t match,uint32_t& id,uint32_t& data){auto* l=find(h);if(!l)return false;for(auto i=l->queue.begin();i!=l->queue.end();++i)if(!match || i->first==match){id=i->first;data=i->second;l->queue.erase(i);return true;}return false;}
    bool close(uint32_t h){auto* l=find(h);if(!l)return false;l->live=false;l->queue.clear();return true;}
};
