#pragma once
#include "core/unattend_generator.hpp"
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

namespace litewin {
// The mount root must already be owned by the caller. Refuse symlink components.
inline bool write_final_unattend(const std::filesystem::path& mounted_windows,
                                 const UnattendOptions& options) {
    int dir = ::open(mounted_windows.c_str(), O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (dir < 0) return false;
    for (const char* part : {"Windows", "Panther"}) {
        if (::mkdirat(dir, part, 0700) != 0 && errno != EEXIST) { ::close(dir); return false; }
        int next = ::openat(dir, part, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        ::close(dir); dir = next;
        if (dir < 0) return false;
    }
    static std::atomic<unsigned long> serial{0};
    const auto temp = ".unattend-" + std::to_string(::getpid()) + "-" + std::to_string(serial.fetch_add(1)) + ".tmp";
    int fd = ::openat(dir, temp.c_str(), O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0600);
    if (fd < 0) { ::close(dir); return false; }
    const auto xml = UnattendGenerator::generate_xml(options);
    size_t offset = 0;
    bool ok = true;
    while (offset < xml.size()) {
        auto n = ::write(fd, xml.data()+offset, xml.size()-offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok=false; break; }
        offset += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) ok=false;
    if (::close(fd) != 0) ok=false;
    if (ok && ::renameat(dir, temp.c_str(), dir, "unattend.xml") != 0) ok=false;
    if (ok && ::fsync(dir) != 0) ok=false;
    if (ok) {
        fd=::openat(dir,"unattend.xml",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0) ok=false;
        else {
            std::string actual; char buf[4096]; ssize_t n;
            while((n=::read(fd,buf,sizeof(buf)))>0) actual.append(buf,static_cast<size_t>(n));
            if(n<0 || actual!=xml) ok=false;
            if(::close(fd)!=0) ok=false;
        }
    }
    ::unlinkat(dir,temp.c_str(),0);
    ::close(dir);
    return ok;
}
} // namespace litewin
