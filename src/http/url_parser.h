#pragma once
#include <iostream>
#include <string_view>
#include <string>

struct UrlParts{
    std::string scheme {"error"}; 
    std::string host {"error"};
    std::string path {"/"};
};

UrlParts parseUrl_(const std::string_view raw_);
