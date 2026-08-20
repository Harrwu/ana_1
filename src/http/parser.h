#pragma once 
#include <iostream>
#include <string>
#include <string_view>
#include <fstream>
#include <optional>
#include <chrono>
#include <format>
#include <map>
#include "ana_1/response.h"
#include "parser_base.h"

class Parse_: public TextParser {
private: 
    std::string_view raw_payload{};
    std::string raw_head{};
    Response resObj;
    bool checkCol(const std::string_view line) const;

    bool isParsed {false};
    bool isConetentLen {false};

    size_t contlen {};
public:
    Parse_(const std::string& pass_payload): TextParser(pass_payload), raw_payload(pass_payload) {};
    bool extract() override;

    void const debug();
    bool writeRawPay() const;
    bool writeBody() const;
    bool writeHead() const;

    void parseHead(std::string_view pass_line);
};
