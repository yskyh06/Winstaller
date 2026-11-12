#pragma once
#include "core/update_protocol.hpp"
#include <functional>
#include <stop_token>
namespace litewin {
using UpdateProgress = std::function<void(std::uint64_t,std::uint64_t)>;
void fetch_official_image(const UpdateComponent&,int,std::stop_token,const UpdateProgress&);
std::string fetch_update_metadata(const std::string& url, std::stop_token stop);
void fetch_update_component(const UpdateComponent& component, int destination_fd,
                            std::stop_token stop, const UpdateProgress& progress);
} // namespace litewin
