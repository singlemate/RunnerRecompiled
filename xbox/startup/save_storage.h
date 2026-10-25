#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
// Persistent Xbox content storage (saved games) on the host file system.
// Layout: <root>/<xuid hex16>/<title hex8>/<content type hex8>/<file name, lowercase>/
// with "<file name>.xcontent" metadata next to each content directory. Content is
// kept apart from game resources, settings, drivers and shader caches. Every host
// error is reported back; nothing reports success before the host operation did.
namespace me_save {
constexpr uint32_t kOk = 0, kFileNotFound = 2, kPathNotFound = 3, kAccessDenied = 5,
                   kNoMoreFiles = 18, kSharingViolation = 32, kNotSupported = 50, kDiskFull = 112, kInvalidName = 123,
                   kAlreadyExists = 183, kInvalidParameter = 87, kFunctionFailed = 1627;
inline uint32_t win32_from_errno(int e) {
  switch (e) {
    case ENOENT: return kPathNotFound;
    case EEXIST: return kAlreadyExists;
    case EACCES: case EPERM: case EROFS: return kAccessDenied;
    case ENOSPC: case EDQUOT: return kDiskFull;
    case EBUSY: return kSharingViolation;
    default: return kFunctionFailed;
  }
}
inline uint32_t ntstatus_from_errno(int e) {
  switch (e) {
    case ENOENT: return 0xC0000034;            // OBJECT_NAME_NOT_FOUND
    case EEXIST: return 0xC0000035;            // OBJECT_NAME_COLLISION
    case ENOTDIR: return 0xC0000103;           // NOT_A_DIRECTORY
    case EISDIR: return 0xC00000BA;            // FILE_IS_A_DIRECTORY
    case ENOTEMPTY: return 0xC0000101;         // DIRECTORY_NOT_EMPTY
    case EACCES: case EPERM: case EROFS: return 0xC0000022;  // ACCESS_DENIED
    case ENOSPC: case EDQUOT: return 0xC000007F;              // DISK_FULL
    case EMFILE: case ENFILE: return 0xC000011F;              // TOO_MANY_OPENED_FILES
    case ENAMETOOLONG: case EINVAL: return 0xC0000033;        // OBJECT_NAME_INVALID
    default: return 0xC00000E9;                               // UNEXPECTED_IO_ERROR
  }
}
// One path component: printable ASCII without separators or wildcards.
inline bool valid_name(const std::string& s, size_t max = 42) {
  if (s.empty() || s.size() > max || s == "." || s == "..") return false;
  for (unsigned char c : s) {
    if (c < 32 || c > 126 || std::strchr("\\/:*?\"<>|", c)) return false;
  }
  return true;
}
inline std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return s;
}
inline bool equal_nocase(const std::string& a, const std::string& b) { return lower(a) == lower(b); }
inline bool write_all(int fd, const void* data, size_t size, uint64_t offset) {
  auto* p = static_cast<const uint8_t*>(data);
  while (size) {
    ssize_t n = pwrite(fd, p, size, off_t(offset));
    if (n < 0) { if (errno == EINTR) continue; return false; }
    if (n == 0) { errno = EIO; return false; }
    p += n; size -= size_t(n); offset += uint64_t(n);
  }
  return true;
}
inline bool fsync_path(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (fd < 0) return false;
  bool ok = fsync(fd) == 0;
  int saved = errno;
  ::close(fd);
  errno = saved;
  return ok;
}
inline std::string parent(const std::string& path) { return path.substr(0, path.rfind('/')); }
// Temporary file, data sync, rename, directory sync: either the old or the new
// metadata is visible after a crash, never a partial one.
inline bool atomic_write(const std::string& path, const std::string& data) {
  std::string tmp = path + ".tmp.XXXXXX";
  int fd = ::mkstemp(tmp.data());
  if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (fd < 0) return false;
  bool ok = write_all(fd, data.data(), data.size(), 0) && fsync(fd) == 0;
  int saved = errno;
  if (::close(fd) != 0 && ok) { ok = false; saved = errno; }
  if (ok && std::rename(tmp.c_str(), path.c_str()) != 0) { ok = false; saved = errno; }
  if (!ok) { ::unlink(tmp.c_str()); errno = saved; return false; }
  return fsync_path(parent(path));
}
inline bool make_dirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      std::string part = path.substr(0, i);
      if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
      struct stat component{};
      if (::lstat(part.c_str(), &component) != 0) return false;
      if (!S_ISDIR(component.st_mode)) { errno = ENOTDIR; return false; }
    }
  }
  struct stat info{};
  return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}
inline bool remove_tree(const std::string& path) {
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0) return errno == ENOENT;
  if (S_ISDIR(info.st_mode)) {
    DIR* dir = ::opendir(path.c_str());
    if (!dir) return false;
    while (dirent* entry = ::readdir(dir)) {
      std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      if (!remove_tree(path + "/" + name)) { int saved = errno; ::closedir(dir); errno = saved; return false; }
    }
    ::closedir(dir);
    return ::rmdir(path.c_str()) == 0;
  }
  return ::unlink(path.c_str()) == 0;
}
}  // namespace me_save

struct MeContentKey {
  uint64_t xuid = 0;
  uint32_t title = 0, type = 0;
  std::string file_name;  // As the guest wrote it (case preserved in metadata).
};
struct MeContentInfo {
  std::string file_name;
  std::u16string display_name;
  uint64_t creator = 0;
};

struct MeSaveStorage {
  std::string root;
  struct Mount { std::string name, dir; bool live = true; };
  struct File {
    int fd = -1;
    std::string path;
    size_t mount = 0;
    uint64_t position = 0;
    bool writable = false, directory = false, delete_on_close = false;
  };
  std::vector<Mount> mounts;
  std::vector<File> files;
  static constexpr uint32_t kHandleBase = 0x70000;

  ~MeSaveStorage() { for (auto& f : files) if (f.fd >= 0) ::close(f.fd); }
  bool init(const std::string& directory) {
    if(directory.empty() || directory.front()!='/'){errno=EINVAL;return false;}
    root = directory;
    return me_save::make_dirs(root);
  }
  static char hex(unsigned v) { return "0123456789ABCDEF"[v & 15]; }
  static std::string hex_string(uint64_t v, unsigned digits) {
    std::string s(digits, '0');
    for (unsigned i = 0; i < digits; ++i) s[digits - 1 - i] = hex(unsigned(v >> (4 * i)));
    return s;
  }
  std::string type_dir(const MeContentKey& k) const {
    return root + "/" + hex_string(k.xuid, 16) + "/" + hex_string(k.title, 8) + "/" + hex_string(k.type, 8);
  }
  std::string content_dir(const MeContentKey& k) const { return type_dir(k) + "/" + me_save::lower(k.file_name); }
  std::string meta_path(const MeContentKey& k) const { return content_dir(k) + ".xcontent"; }
  bool exists(const MeContentKey& k) const {
    struct stat info{};
    return ::stat(content_dir(k).c_str(), &info) == 0 && S_ISDIR(info.st_mode);
  }
  static std::string encode_meta(const MeContentInfo& info) {
    std::string out = "version=1\nfile_name=" + info.file_name + "\ncreator=" + hex_string(info.creator, 16) + "\ndisplay=";
    for (char16_t c : info.display_name) out += hex_string(uint16_t(c), 4);
    return out + "\n";
  }
  bool read_meta(const MeContentKey& k, MeContentInfo& info) const {
    int fd=::open(meta_path(k).c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
    FILE* f=::fdopen(fd,"rb");if(!f){::close(fd);return false;}
    char line[1024];bool version=false,name=false,creator=false,display=false,valid=true;info={};
    auto hex_digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;};
    while(std::fgets(line,sizeof(line),f)){
      std::string value(line);if(value.empty() || value.back()!='\n'){valid=false;break;}value.pop_back();
      if(value=="version=1" && !version)version=true;
      else if(value.rfind("file_name=",0)==0 && !name){name=true;info.file_name=value.substr(10);}
      else if(value.rfind("creator=",0)==0 && !creator){creator=true;if(value.size()!=24){valid=false;break;}
        for(size_t i=8;i<24;++i){int digit=hex_digit(value[i]);if(digit<0){valid=false;break;}info.creator=info.creator*16+unsigned(digit);}}
      else if(value.rfind("display=",0)==0 && !display){display=true;if((value.size()-8)%4 || value.size()>8+128*4){valid=false;break;}
        for(size_t i=8;i<value.size();i+=4){uint16_t c=0;for(unsigned j=0;j<4;++j){int digit=hex_digit(value[i+j]);if(digit<0){valid=false;break;}c=uint16_t(c*16+digit);}info.display_name.push_back(char16_t(c));}}
      else{valid=false;break;}
      if(!valid)break;
    }
    valid=valid && !std::ferror(f);std::fclose(f);
    return valid && version && name && creator && display && me_save::valid_name(info.file_name) && me_save::equal_nocase(info.file_name,k.file_name) && info.creator==k.xuid;
  }
  Mount* find_mount(const std::string& name) {
    for (auto& m : mounts) if (m.live && me_save::equal_nocase(m.name, name)) return &m;
    return nullptr;
  }
  bool mounted(const MeContentKey& k) const {
    const std::string dir = content_dir(k);
    for (const auto& m : mounts) if (m.live && m.dir == dir) return true;
    return false;
  }
  // XamContentCreate semantics; flags & 0xF: 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING,
  // 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING. disposition_out: 1 created, 2 opened. Win32 result.
  uint32_t create(const MeContentKey& k, const std::u16string& display_name, uint32_t flags,
                  const std::string& mount_name, uint32_t& disposition_out) {
    disposition_out = 0;
    if (display_name.size()>128 || !me_save::valid_name(k.file_name) || !me_save::valid_name(mount_name, 32)) return me_save::kInvalidName;
    if (find_mount(mount_name)) return me_save::kAlreadyExists;
    const bool present = exists(k);
    if(present){MeContentInfo existing;if(!read_meta(k,existing))return me_save::kFunctionFailed;}
    bool create_new = false;
    switch (flags & 0xF) {
      case 1: if (present) return me_save::kAlreadyExists; create_new = true; break;
      case 2: case 5:
        if ((flags & 0xF) == 5 && !present) return me_save::kPathNotFound;
        if (present) {
          if (mounted(k)) return me_save::kSharingViolation;
          // No destructive replace before a crash-safe generation/recovery protocol exists.
          // Preserve the old container and report a real unsupported operation.
          return me_save::kNotSupported;
        }
        create_new = true;
        break;
      case 3: if (!present) return me_save::kPathNotFound; break;
      case 4: create_new = !present; break;
      default: return me_save::kInvalidParameter;
    }
    if (create_new) {
      if (!me_save::make_dirs(type_dir(k))) return me_save::win32_from_errno(errno);
      MeContentInfo info{k.file_name, display_name, k.xuid};
      // Metadata first (atomic), then the directory that marks the content as present.
      if (!me_save::atomic_write(meta_path(k), encode_meta(info))) return me_save::win32_from_errno(errno);
      if (::mkdir(content_dir(k).c_str(), 0700) != 0 && errno != EEXIST) return me_save::win32_from_errno(errno);
      if (!me_save::fsync_path(type_dir(k))) return me_save::win32_from_errno(errno);
    }
    mounts.push_back({mount_name, content_dir(k), true});
    disposition_out = create_new ? 1 : 2;
    return me_save::kOk;
  }
  uint32_t flush_mount(const std::string& name) {
    Mount* m = find_mount(name);
    if (!m) return me_save::kPathNotFound;
    const size_t index = size_t(m - mounts.data());
    for (auto& f : files) {
      if (f.fd >= 0 && f.mount == index && f.writable && fsync(f.fd) != 0) return me_save::win32_from_errno(errno);
    }
    return sync_tree(m->dir) ? me_save::kOk : me_save::win32_from_errno(errno);
  }
  uint32_t close_mount(const std::string& name) {
    auto* mount=find_mount(name);
    if(!mount)return me_save::kPathNotFound;
    const auto index=size_t(mount-mounts.data());
    for(const auto& file:files)if(file.fd>=0 && file.mount==index)return me_save::kSharingViolation;
    const uint32_t flushed = flush_mount(name);
    if (flushed != me_save::kOk) return flushed;
    find_mount(name)->live = false;
    return me_save::kOk;
  }
  uint32_t remove(const MeContentKey& k) {
    if (!exists(k)) return me_save::kPathNotFound;
    if (mounted(k)) return me_save::kSharingViolation;
    if (!me_save::remove_tree(content_dir(k))) return me_save::win32_from_errno(errno);
    if (::unlink(meta_path(k).c_str()) != 0 && errno != ENOENT) return me_save::win32_from_errno(errno);
    return me_save::fsync_path(type_dir(k)) ? me_save::kOk : me_save::win32_from_errno(errno);
  }
  // Content of one user/title/type, sorted by host directory name.
  std::vector<MeContentInfo> list(uint64_t xuid, uint32_t title, uint32_t type, uint32_t* error=nullptr) const {
    std::vector<MeContentInfo> out;
    if(error)*error=0;
    MeContentKey k{xuid,title,type,{}};
    DIR* dir=::opendir(type_dir(k).c_str());
    if(!dir){if(error && errno!=ENOENT)*error=me_save::win32_from_errno(errno);return out;}
    std::vector<std::string> names;uint32_t failure=0;
    for(;;){
      errno=0;dirent* entry=::readdir(dir);
      if(!entry){if(errno)failure=me_save::win32_from_errno(errno);break;}
      std::string name=entry->d_name;
      if(name=="." || name==".." || !me_save::valid_name(name))continue;
      struct stat info{};
      if(::lstat((type_dir(k)+"/"+name).c_str(),&info)){failure=me_save::win32_from_errno(errno);break;}
      if(S_ISLNK(info.st_mode)){failure=me_save::kAccessDenied;break;}
      if(S_ISDIR(info.st_mode))names.push_back(name);
    }
    ::closedir(dir);std::sort(names.begin(),names.end());
    if(!failure)for(const auto& name:names){
      k.file_name=name;MeContentInfo info;
      if(!read_meta(k,info)){failure=me_save::kFunctionFailed;break;}
      out.push_back(info);
    }
    if(failure){out.clear();if(error)*error=failure;}
    return out;
  }
  bool sync_tree(const std::string& path) {
    if (!me_save::fsync_path(path)) return false;
    DIR* dir = ::opendir(path.c_str());
    if (!dir) return false;
    bool ok = true;
    while (dirent* entry = ::readdir(dir)) {
      std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      struct stat info{};
      std::string child = path + "/" + name;
      if (::lstat(child.c_str(), &info) != 0) { ok = false; break; }
      if (S_ISDIR(info.st_mode)) { if(!sync_tree(child)){ok=false;break;} }
      else if(S_ISREG(info.st_mode)){
        int fd=::open(child.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0){ok=false;break;}
        const int result=::fsync(fd),saved=errno;::close(fd);errno=saved;
        if(result){ok=false;break;}
      }else{errno=EINVAL;ok=false;break;}
    }
    int saved = errno;
    ::closedir(dir);
    errno = saved;
    return ok;
  }
  // Case-insensitive, case-preserving lookup of one component inside a directory.
  static std::string match(const std::string& dir, const std::string& name) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return name;
    std::string found = name;
    while (dirent* entry = ::readdir(d)) {
      if (me_save::equal_nocase(entry->d_name, name)) { found = entry->d_name; break; }
    }
    ::closedir(d);
    return found;
  }
  // Splits "a\\b\\c" (inside a mount) into a host path. NTSTATUS result.
  uint32_t resolve(const Mount& m, const std::string& relative, std::string& host) const {
    host = m.dir;
    std::string part;
    for (size_t i = 0; i <= relative.size(); ++i) {
      char c = i == relative.size() ? '\\' : relative[i];
      if (c == '\\' || c == '/') {
        if (part.empty()) { if (i == relative.size()) break; continue; }
        if (!me_save::valid_name(part, 255)) return 0xC0000033;
        host += "/" + match(host, part);
        struct stat item{};
        if(::lstat(host.c_str(),&item)==0){if(S_ISLNK(item.st_mode))return 0xC0000022;}
        else if(errno!=ENOENT)return me_save::ntstatus_from_errno(errno);
        part.clear();
      } else {
        part += c;
      }
    }
    return 0;
  }
  File* find(uint32_t handle) {
    if (handle < kHandleBase || (handle - kHandleBase) % 4) return nullptr;
    size_t i = (handle - kHandleBase) / 4;
    return i < files.size() && files[i].fd >= 0 ? &files[i] : nullptr;
  }
  // NtCreateFile inside a live mount. disposition: 0 SUPERSEDE, 1 OPEN, 2 CREATE, 3 OPEN_IF,
  // 4 OVERWRITE, 5 OVERWRITE_IF. options: 0x1 DIRECTORY, 0x40 NON_DIRECTORY, 0x1000 DELETE_ON_CLOSE.
  // info_out: IO_STATUS_BLOCK.Information. Returns NTSTATUS.
  uint32_t open(Mount& m, const std::string& relative, uint32_t disposition, bool write, uint32_t options,
                uint32_t& handle_out, uint32_t& info_out) {
    handle_out = 0;
    info_out = 0;
    if (disposition > 5) return 0xC000000D;
    std::string host;
    if (uint32_t status = resolve(m, relative, host)) return status;
    if (host != m.dir) {
      struct stat parent_info{};
      if (::stat(me_save::parent(host).c_str(), &parent_info) != 0 || !S_ISDIR(parent_info.st_mode)) return 0xC000003A;
    }
    struct stat info{};
    const bool present = ::stat(host.c_str(), &info) == 0;
    const bool want_dir = options & 0x1, want_file = options & 0x40;
    if (present && want_dir && !S_ISDIR(info.st_mode)) return 0xC0000103;
    if (present && want_file && S_ISDIR(info.st_mode)) return 0xC00000BA;
    if (present && disposition == 2) return 0xC0000035;
    if (!present && (disposition == 1 || disposition == 4)) return 0xC0000034;
    size_t slot=0;while(slot<files.size() && files[slot].fd>=0)++slot;
    if (slot>=256) return 0xC000011F;
    File f;
    f.mount = size_t(&m - mounts.data());
    f.path = host;
    f.delete_on_close = options & 0x1000;
    if (want_dir || (present && S_ISDIR(info.st_mode))) {
      if (!present && ::mkdir(host.c_str(), 0700) != 0) return me_save::ntstatus_from_errno(errno);
      if (!present && !me_save::fsync_path(me_save::parent(host))) return me_save::ntstatus_from_errno(errno);
      f.fd = ::open(host.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      f.directory = true;
      info_out = present ? 1 : 2;
    } else {
      int flags = O_CLOEXEC | O_NOFOLLOW | (write ? O_RDWR : O_RDONLY);
      if (!present) flags |= O_CREAT | O_EXCL;
      else if (disposition == 0 || disposition == 4 || disposition == 5) flags |= O_TRUNC;
      if ((flags & O_TRUNC) && !write) return 0xC0000022;
      f.fd = ::open(host.c_str(), flags, 0600);
      f.writable = write;
      info_out = !present ? 2 : (disposition == 0 ? 0 : (flags & O_TRUNC) ? 3 : 1);
      if (f.fd >= 0 && !present && !me_save::fsync_path(me_save::parent(host))) {
        int saved = errno;
        ::close(f.fd);
        return me_save::ntstatus_from_errno(saved);
      }
    }
    if (f.fd < 0) return me_save::ntstatus_from_errno(errno);
    if(slot==files.size())files.push_back(f);else files[slot]=f;
    handle_out = kHandleBase + uint32_t(slot) * 4;
    return 0;
  }
  // Byte counts on success, -1 with errno on failure.
  int64_t read(File& f, void* data, uint32_t length, uint64_t offset) {
    if(offset>uint64_t(INT64_MAX) || uint64_t(length)>uint64_t(INT64_MAX)-offset){errno=EINVAL;return -1;}
    ssize_t n;
    do { n = pread(f.fd, data, length, off_t(offset)); } while (n < 0 && errno == EINTR);
    if (n >= 0) f.position = offset + uint64_t(n);
    return n;
  }
  bool write(File& f, const void* data, uint32_t length, uint64_t offset) {
    if(offset>uint64_t(INT64_MAX) || uint64_t(length)>uint64_t(INT64_MAX)-offset){errno=EINVAL;return false;}
    if (!f.writable) { errno = EACCES; return false; }
    if (!me_save::write_all(f.fd, data, length, offset)) return false;
    f.position = offset + length;
    return true;
  }
  uint32_t close(uint32_t handle) {
    File* f = find(handle);
    if (!f) return 0xC0000008;
    int fd = f->fd;
    f->fd = -1;
    uint32_t status = 0;
    if (f->writable && fsync(fd) != 0) status = me_save::ntstatus_from_errno(errno);
    if (::close(fd) != 0 && !status) status = me_save::ntstatus_from_errno(errno);
    if (f->delete_on_close && !status) {
      if ((f->directory ? ::rmdir(f->path.c_str()) : ::unlink(f->path.c_str())) != 0) status = me_save::ntstatus_from_errno(errno);
      else if (!me_save::fsync_path(me_save::parent(f->path))) status = me_save::ntstatus_from_errno(errno);
    }
    return status;
  }
  bool space(uint64_t& total, uint64_t& free) const {
    struct statvfs info{};
    if (::statvfs(root.c_str(), &info) != 0) return false;
    total = uint64_t(info.f_blocks) * info.f_frsize;
    free = uint64_t(info.f_bavail) * info.f_frsize;
    return true;
  }
};
