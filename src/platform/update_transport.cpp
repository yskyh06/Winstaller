#include "platform/update_transport.hpp"
#include <curl/curl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace litewin {
namespace {
bool public_v4(std::uint32_t n) {
    return (n>>24)!=0 && (n>>24)!=10 && (n>>24)!=127 && (n>>24)<224 &&
        (n>>16)!=0xa9fe && (n>>16)!=0xc0a8 && (n>>20)!=0xac1 && (n>>22)!=0x191 && (n>>17)!=0x6309;
}
curl_socket_t open_public_socket(void*, curlsocktype purpose, curl_sockaddr* address) {
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKET_BAD;
    if (address->family == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(&address->addr);
        if (!public_v4(ntohl(a->sin_addr.s_addr))) return CURL_SOCKET_BAD;
    } else if (address->family == AF_INET6) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&address->addr);
        const auto* b = a->sin6_addr.s6_addr;
        if (IN6_IS_ADDR_LOOPBACK(&a->sin6_addr) || IN6_IS_ADDR_UNSPECIFIED(&a->sin6_addr) ||
            IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr) || IN6_IS_ADDR_MULTICAST(&a->sin6_addr) || (b[0]&0xfe)==0xfc) return CURL_SOCKET_BAD;
        if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) { std::uint32_t v; std::memcpy(&v,b+12,4); if(!public_v4(ntohl(v))) return CURL_SOCKET_BAD; }
        else if ((b[0]&0xe0)!=0x20 || (b[0]==0x20 && b[1]==0x02)) return CURL_SOCKET_BAD;
    } else return CURL_SOCKET_BAD;
    return socket(address->family,address->socktype | SOCK_CLOEXEC,address->protocol);
}
struct Transfer {
    CURL* handle = nullptr;
    std::string body;
    int fd = -1;
    std::uint64_t limit = 0, offset = 0, written = 0;
    bool failed = false, html = false;
    std::stop_token stop;
    UpdateProgress progress;
};
size_t write_data(char* data,size_t size,size_t count,void* pointer) {
    auto& t = *static_cast<Transfer*>(pointer);
    const size_t bytes = size*count;
    long code = 0; curl_easy_getinfo(t.handle,CURLINFO_RESPONSE_CODE,&code);
    if (code >= 300 && code < 400) return bytes;
    if (code != 200 && code != 206) return bytes;
    if (t.stop.stop_requested() || t.html || bytes > t.limit-t.offset-t.written) {t.failed=true;return 0;}
    try {
        if (t.fd < 0) t.body.append(data,bytes);
        else {
            size_t copied=0;
            while(copied<bytes) {
                const auto n=write(t.fd,data+copied,bytes-copied);
                if(n<0 && errno==EINTR)continue;
                if(n<=0) {t.failed=true;return 0;}
                copied+=static_cast<size_t>(n);
            }
        }
        t.written+=bytes;
        return bytes;
    } catch (...) {t.failed=true;return 0;}
}
size_t read_header(char* data,size_t size,size_t count,void* pointer) {
    auto& t=*static_cast<Transfer*>(pointer);
    std::string line(data,size*count);
    std::transform(line.begin(),line.end(),line.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(line.starts_with("content-type:") && line.find("text/html")!=std::string::npos)t.html=true;
    return size*count;
}
int transfer_progress(void* pointer,curl_off_t total,curl_off_t now,curl_off_t,curl_off_t) {
    auto& t=*static_cast<Transfer*>(pointer);
    if(t.stop.stop_requested())return 1;
    try {if(t.progress)t.progress(t.offset+static_cast<std::uint64_t>(std::max<curl_off_t>(0,now)),total>0?t.offset+static_cast<std::uint64_t>(total):t.limit);}
    catch(...){return 1;}
    return 0;
}
void transfer(const std::string& first,Transfer& t,bool metadata,bool image=false) {
    auto url=first;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    bool retried_range=false;
    for(int hop=0;hop<=3;++hop) {
        if(!valid_update_url(url,image))throw std::runtime_error("Untrusted update redirect");
        if(t.stop.stop_requested())throw std::runtime_error("Update cancelled");
        std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
        if(!curl)throw std::runtime_error("Could not initialize update connection");
        t.handle=curl.get(); t.written=0;t.failed=false;t.html=false;
        curl_easy_setopt(curl.get(),CURLOPT_URL,url.c_str());
        curl_easy_setopt(curl.get(),CURLOPT_PROTOCOLS_STR,"https");
        curl_easy_setopt(curl.get(),CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(curl.get(),CURLOPT_PROXY,"");
        curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYPEER,1L);
        curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYHOST,2L);
        curl_easy_setopt(curl.get(),CURLOPT_CONNECTTIMEOUT,5L);
        curl_easy_setopt(curl.get(),CURLOPT_LOW_SPEED_LIMIT,1024L);
        curl_easy_setopt(curl.get(),CURLOPT_LOW_SPEED_TIME,30L);
        curl_easy_setopt(curl.get(),CURLOPT_OPENSOCKETFUNCTION,open_public_socket);
        curl_easy_setopt(curl.get(),CURLOPT_WRITEFUNCTION,write_data);
        curl_easy_setopt(curl.get(),CURLOPT_WRITEDATA,&t);
        curl_easy_setopt(curl.get(),CURLOPT_HEADERFUNCTION,read_header);
        curl_easy_setopt(curl.get(),CURLOPT_HEADERDATA,&t);
        curl_easy_setopt(curl.get(),CURLOPT_XFERINFOFUNCTION,transfer_progress);
        curl_easy_setopt(curl.get(),CURLOPT_XFERINFODATA,&t);
        curl_easy_setopt(curl.get(),CURLOPT_NOPROGRESS,0L);
        curl_easy_setopt(curl.get(),CURLOPT_NOSIGNAL,1L);
        if(metadata) {
            const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
            if(ms<=0)throw std::runtime_error("Update check timed out");
            curl_easy_setopt(curl.get(),CURLOPT_TIMEOUT_MS,static_cast<long>(ms));
        }
        if(t.offset)curl_easy_setopt(curl.get(),CURLOPT_RESUME_FROM_LARGE,static_cast<curl_off_t>(t.offset));
        const auto result=curl_easy_perform(curl.get());
        long code=0;curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&code);
        if(result==CURLE_RANGE_ERROR && t.fd>=0 && t.offset && !retried_range) {
            if(ftruncate(t.fd,0)!=0 || lseek(t.fd,0,SEEK_SET)<0)throw std::runtime_error("Could not restart component download");
            t.offset=0;retried_range=true;--hop;continue;
        }
        if(result!=CURLE_OK || t.failed || (t.html && !(code>=300 && code<400)))throw std::runtime_error(t.stop.stop_requested()?"Update cancelled":"Update download failed verification or transfer limits");
        if(code>=300 && code<400) {
            char* next=nullptr;curl_easy_getinfo(curl.get(),CURLINFO_REDIRECT_URL,&next);
            if(!next || hop==3)throw std::runtime_error("Update redirect limit exceeded");
            url=next;continue;
        }
        if(code!=200 && code!=206)throw std::runtime_error("Update server request failed");
        if(t.fd>=0 && t.offset+t.written!=t.limit)throw std::runtime_error("Incomplete component download");
        return;
    }
    throw std::runtime_error("Update redirect limit exceeded");
}
}
std::string fetch_update_metadata(const std::string& url,std::stop_token stop) {
    Transfer t;t.limit=2*1024*1024;t.stop=stop;transfer(url,t,true);return t.body;
}
static void fetch_component(const UpdateComponent& component,int fd,std::stop_token stop,const UpdateProgress& progress,bool image) {
    const auto offset=lseek(fd,0,SEEK_END);
    if(offset<0 || static_cast<std::uint64_t>(offset)>component.size)throw std::runtime_error("Invalid partial update file");
    if(static_cast<std::uint64_t>(offset)==component.size)return;
    Transfer t;t.fd=fd;t.limit=component.size;t.offset=static_cast<std::uint64_t>(offset);t.stop=stop;t.progress=progress;
    transfer(component.url,t,false,image);
    if(fsync(fd)!=0)throw std::runtime_error("Could not persist component download");
}
void fetch_update_component(const UpdateComponent& c,int fd,std::stop_token stop,const UpdateProgress& progress){fetch_component(c,fd,stop,progress,false);}
void fetch_official_image(const UpdateComponent& c,int fd,std::stop_token stop,const UpdateProgress& progress){fetch_component(c,fd,stop,progress,true);}
} // namespace litewin
