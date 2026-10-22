#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <fstream>
#include <unordered_set>
struct MeReadOnlyFiles {
    struct File { int fd; uint64_t size, position; };
    std::string root;
    std::vector<File> files;
    std::unordered_set<std::string> known_paths;
    bool inventory_loaded = false;
    explicit MeReadOnlyFiles(std::string directory):root(std::move(directory)){}
    ~MeReadOnlyFiles(){for(auto& f:files)if(f.fd>=0)::close(f.fd);}
    static bool relative(std::string path,std::string& result) {
        result.clear();
        if(path.size()<4 || (path[0]!='D' && path[0]!='d') || path[1]!=':' || (path[2]!='\\' && path[2]!='/'))return false;
        std::string part;
        for(size_t i=3;i<=path.size();++i){
            char c=i==path.size()?'/':path[i];
            if(c=='\\'||c=='/'){
                if(part.empty()||part=="."||part=="..")return false;
                if(!result.empty())result+='/';result+=part;part.clear();
            }else{
                if((unsigned char)c<32 || (unsigned char)c>126 || c==':' || c=='"')return false;
                part+=c>='A'&&c<='Z'?char(c+'a'-'A'):c;
            }
        }
        return !result.empty();
    }
    File* find(uint32_t handle){
        if(handle<0x30000 || (handle-0x30000)%4)return nullptr;
        size_t i=(handle-0x30000)/4;
        return i<files.size() && files[i].fd>=0 ? &files[i] : nullptr;
    }
    uint32_t open(const std::string& path){
        std::string name;if(!relative(path,name)){errno=EINVAL;return 0;}
        if(!inventory_loaded){
            std::ifstream index(root+"/inventory.txt");
            if(!index){errno=ENODATA;return 0;}
            std::string entry;
            while(std::getline(index,entry)){
                if(!entry.empty() && entry.back()=='\r')entry.pop_back();
                std::string normalized;
                if(known_paths.size()>500000 || !relative("D:/"+entry,normalized) || normalized!=entry){known_paths.clear();errno=EINVAL;return 0;}
                known_paths.insert(entry);
            }
            if(!index.eof() || known_paths.empty()){known_paths.clear();errno=EINVAL;return 0;}
            inventory_loaded=true;
        }
        if(!known_paths.count(name)){errno=ENOENT;return 0;}
        if(files.size()>=256){errno=EMFILE;return 0;}
        int fd=::open((root+"/"+name).c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0){if(errno==ENOENT)errno=ENODATA;return 0;}
        struct stat info{};
        if(fstat(fd,&info)||!S_ISREG(info.st_mode)||info.st_size<0){::close(fd);errno=EINVAL;return 0;}
        files.push_back({fd,uint64_t(info.st_size),0});
        return 0x30000+uint32_t(files.size()-1)*4;
    }
    int64_t read(uint32_t handle,void* data,uint32_t length,uint64_t offset){
        auto* f=find(handle);if(!f || offset>INT64_MAX){errno=EINVAL;return -1;}
        ssize_t n;
        do {n=pread(f->fd,data,length,off_t(offset));}while(n<0&&errno==EINTR);
        if(n>=0)f->position=offset+uint64_t(n);
        return n;
    }
    bool close(uint32_t handle){auto* f=find(handle);if(!f)return false;int fd=f->fd;f->fd=-1;return ::close(fd)==0;}
};
