#pragma once
#include "parser_base.h"
#include <string_view>
#include <iostream>
#include <string>
#include <fstream>
#include <filesystem>
#include <set>
#include <array>
#include <utility>
#include <chrono>
#include <format>
#include <cstring>
#include <vector>

class HtmlParse_: public TextParser {
private:
    std::string_view raw_body{};
    std::string _path {}, _file {}; 

    
    const std::array<std::string, 14> arr = {
        "<h1", "<h2", "<h3",
        "<h4", "<h5", "<h6", "<p",
        "</h1>", "</h2>", "</h3>", 
        "</h4>", "</h5>", "</h6>", "</p>"
    };

    bool isFile {true}, isOpenFile {false};

    const std::string rePull(std::string_view extra_) const;
public:
    HtmlParse_(const std::string& pass_raw) : TextParser(pass_raw), raw_body(pass_raw){} 
    bool extract() override;
    void mainLnk(std::ofstream& link_file) const;
};
