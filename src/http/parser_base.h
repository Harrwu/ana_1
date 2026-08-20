#pragma once
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


class TextParser{
protected:
    std::string_view raw_body {};
    std::string _path{}, _file {};
    bool isFile = true, isOpenFile {false};
public:
    TextParser(const std::string& pass_raw): raw_body(pass_raw) {
        auto const now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
        _file = std::format("../_output/{:%Y-%m-%d}", now);
        try { std::filesystem::create_directories(_file); isFile = true;}
        catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Filesystem: " << e.what() << "\n";
            isFile = false;
        }
    }
    virtual ~TextParser() = default;

    bool virtual extract() = 0;
};
