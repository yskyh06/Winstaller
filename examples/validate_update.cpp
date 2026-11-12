#include "core/update_protocol.hpp"
#include <ctime>
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv) {
    try {
        if(argc!=4)throw std::runtime_error("Usage: validate_update CONFIG catalog|release SIGNED_JSON");
        auto config=litewin::UpdateConfig::load(argv[1]);
        std::ifstream file(argv[3],std::ios::binary);if(!file)throw std::runtime_error("Cannot open metadata");
        std::string bytes(2*1024*1024+1,'\0');file.read(bytes.data(),bytes.size());bytes.resize(file.gcount());
        if(std::string(argv[2])=="catalog") {
            auto catalog=litewin::verify_catalog(bytes,config,std::time(nullptr));
            std::cout<<"Verified catalog revision "<<catalog.revision<<", "<<catalog.targets.size()<<" targets\n";
        } else if(std::string(argv[2])=="release") {
            auto release=litewin::verify_release(bytes,config,std::time(nullptr));
            if(!release.compatible)throw std::runtime_error(release.incompatible_reason);
            std::cout<<"Verified release "<<release.id<<", sequence "<<release.sequence<<"\n";
        } else throw std::runtime_error("Unknown document role");
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
