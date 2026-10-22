#pragma once
#include <cstdint>
#include <vector>
#include <string>

// Named and unnamed events owned by the single initial guest process. No host pointers
// escape into the guest, and closed handles are not reused. Callers hold
// the runtime execution gate when accessing this model.
struct MeEvents {
    struct Event { bool manual, signaled, live; std::string name; uint32_t references = 1; };
    std::vector<Event> objects;
    Event* find(uint32_t handle) {
        if (handle < 0x10000 || (handle & 3)) return nullptr;
        const auto index = (handle - 0x10000) / 4;
        return index < objects.size() && objects[index].live ? &objects[index] : nullptr;
    }
    uint32_t create(uint32_t type, bool state, uint32_t& handle, const std::string& name = {}, bool insensitive = false) {
        if (type > 1) return 0xc000000d;
        auto equal = [&](const std::string& a, const std::string& b) {
            if(a.size()!=b.size())return false;
            for(size_t i=0;i<a.size();++i){
                char x=a[i], y=b[i];
                if(insensitive){if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;}
                if(x!=y)return false;
            }
            return true;
        };
        if(!name.empty())for(size_t i=0;i<objects.size();++i){auto& event=objects[i];
            if(event.live && equal(event.name,name)){
                if(event.references==UINT32_MAX)return 0xc000009a;
                ++event.references;handle=0x10000+uint32_t(i)*4;return 0x40000000;
            }
        }
        if (objects.size() >= 4096) return 0xc000009a;
        handle = 0x10000 + uint32_t(objects.size()) * 4;
        objects.push_back({type == 0, state, true, name, 1});
        return 0;
    }
    uint32_t set(uint32_t handle, uint32_t& previous) {
        auto* event = find(handle); if (!event) return 0xc0000008;
        previous = event->signaled; event->signaled = true; return 0;
    }
    uint32_t clear(uint32_t handle) {
        auto* event = find(handle); if (!event) return 0xc0000008;
        event->signaled = false; return 0;
    }
    uint32_t close(uint32_t handle) {
        auto* event = find(handle); if (!event) return 0xc0000008;
        if(!--event->references)event->live = false; return 0;
    }
    // false means the operation needs a scheduler; the caller must diagnose it.
    bool wait(uint32_t handle, bool polling, uint32_t& status) {
        auto* event = find(handle);
        if (!event) { status = 0xc0000008; return true; }
        if (event->signaled) {
            if (!event->manual) event->signaled = false;
            status = 0; return true;
        }
        if (polling) { status = 0x102; return true; }
        return false;
    }
};
