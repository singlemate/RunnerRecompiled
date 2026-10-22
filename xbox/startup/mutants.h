#pragma once
#include <cstdint>
#include <vector>
struct MeMutants {
    struct Object {uint32_t owner=0,depth=0;bool live=true,abandoned=false;};
    std::vector<Object> objects;
    Object* find(uint32_t h){if(h<0x40000 || (h&3))return nullptr;auto i=(h-0x40000)/4;return i<objects.size()&&objects[i].live?&objects[i]:nullptr;}
    uint32_t create(uint32_t owner,uint32_t& h){if(objects.size()>=4096)return 0xc000009a;h=0x40000+uint32_t(objects.size())*4;objects.push_back({owner,owner?1u:0u,true,false});return 0;}
    bool wait(uint32_t h,uint32_t thread,uint32_t& status){auto* m=find(h);if(!m){status=0xc0000008;return true;}if(m->owner && m->owner!=thread)return false;if(m->depth==0x7fffffff){status=0xc0000047;return true;}status=m->abandoned?0x80:0;m->abandoned=false;m->owner=thread;++m->depth;return true;}
    uint32_t release(uint32_t h,uint32_t thread){auto* m=find(h);if(!m)return 0xc0000008;if(m->owner!=thread || !m->depth)return 0xc0000046;if(!--m->depth)m->owner=0;return 0;}
    uint32_t close(uint32_t h){auto* m=find(h);if(!m)return 0xc0000008;if(m->owner)return 0xc000000d;m->live=false;return 0;}
    void abandon(uint32_t thread){for(auto& m:objects)if(m.live && m.owner==thread){m.owner=0;m.depth=0;m.abandoned=true;}}
};
