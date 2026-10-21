#pragma once
#include "shared_network.hpp"
namespace radaer {
Json linux_network_inventory();
Json linux_capture_preflight(const CaptureOptions &options);
std::string linux_interface(unsigned index, const std::string &address);
}
