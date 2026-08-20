#pragma once
#include <string_view>
#include <string>
#include <iostream>
#include <fstream>
#include <chrono>
#include <array>
#include <format>
#include <set>
#include "parser_base.h"

class XmlParse_: public TextParser {
private:
    std::string_view raw_body {};
    std::vector<std::string_view> link_res {};

    const std::array<std::string, 4> arr = {
        "<loc>", "<sitemap>",
        "</loc>", "</sitemap>"
    };

public:
    XmlParse_(std::string& pass_body): raw_body(pass_body), TextParser(pass_body) {}
    bool extract() override;
};
