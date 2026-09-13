#include "parser.h"
#include <filesystem>
#include <optional>

bool Parse_::extract(){
    size_t eol = raw_payload.find("\r\n");
    if (eol == std::string_view::npos) {
        std::cerr << "No status_line\n";
        return false;
    }
    
    std::string_view status_line = raw_payload.substr(0, eol);
    std::cout << "Status line: " << status_line << "\n";
    

    size_t prev{eol}, found = raw_payload.find("\r\n", prev + 2);

    while (found != std::string_view::npos) {
        std::string_view curStr = raw_payload.substr(prev + 2, found - (prev + 2));
        if (!checkCol(curStr)) break;

        prev = found;
        parseHead(curStr);

        {
            size_t pass_col = curStr.find("<!doctype html>");
            if (pass_col != std::string_view::npos) break;
        }

        //std::cout << curStr << "\n";
        found = raw_payload.find("\r\n", prev + 2);
    }

    size_t body_start = raw_payload.find("\r\n\r\n");
    
    if (body_start == std::string_view::npos) {
        std::cout << "No body boundary found.\n";
    } else {
        body_start += 4;
        std::string_view raw_body_text = raw_payload.substr(body_start);
        
        size_t pass_ = raw_body_text.find('<');
        if (pass_ == std::string_view::npos) {
            std::cout << "Notice no HTML tags found\n";
            resObj.setBody(std::string(raw_body_text));
        } else {
            std::string_view pure_html = raw_body_text.substr(pass_);
            resObj.setBody(std::string(pure_html));
        }
    }

    isParsed = true;
    return true; 
}

void const Parse_::debug(){
    std::cout << "\n\n";
    for (auto it = raw_payload.begin(); it != raw_payload.end(); it ++) {
        if (*it == '\r') std::cout << "\\r";
        else if (*it == '\n') std::cout << "\\n \n";
        else std::cout << *it;
    }
    std::cout << "\n\n";
}

bool Parse_::checkCol(const std::string_view line) const {
    size_t col {line.find(":")};
    if (col == std::string_view::npos) return false;
    return true;   
}

bool Parse_::writeRawPay() const{
    auto const now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
    std::string time {std::format("../_output/{:%Y-%m-%d %X}/raw.txt", now)};

    std::ofstream out_file(time);
    if (!out_file) return false;
    
    for (auto it = raw_payload.begin(); it != raw_payload.end(); it ++) {
        if (*it == '\r') out_file << "\\r";
        else if (*it == '\n') out_file << "\\n \n";
        else out_file << *it;
    }
    out_file.close();
    return true;
}

void Parse_::parseHead(std::string_view pass_line) {

    size_t col_pos = pass_line.find(':');
    if (col_pos == std::string_view::npos) return;
        
    std::string_view key = pass_line.substr(0, col_pos);

    size_t value_start = col_pos + 2; 
    if (pass_line[col_pos + 1] != ' ') value_start = col_pos + 1;
    
    std::string_view value = pass_line.substr(value_start);

    if (key == "")

    resObj.addHead(std::string(key), std::string(value));

    std::string pass{};
    pass.append(key); pass.append(" : ");
    pass.append(value); pass.append("\n");

    raw_head.append(pass);
}

bool Parse_::writeBody() const{
    if (isParsed) {
        auto const now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
        std::string folder {std::format("../_output/{:%Y-%m-%d}", now)};

        try{
            std::filesystem::create_directories(folder);
            std::string time {std::format("../_output/{0:%Y-%m-%d}/html{0:%H-%M-%S}.txt", now)};

            std::ofstream out_file(time);
            if (!out_file) {std::cerr << "e"; return false;}

            
            out_file << resObj.getBody() << "\n";
            out_file.close();
            return true;

        }catch (const std::filesystem::filesystem_error& e){
            std::cerr << "Filesysten Error: " << e.what() << "\n";
        }
    }
    
    return false;
}

bool Parse_::writeHead() const{
    if (isParsed) {
        auto const now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
        std::string folder {std::format("../_output/{:%Y-%m-%d}", now)};

        try{
            std::filesystem::create_directories(folder);
            std::string time {std::format("../_output/{0:%Y-%m-%d}/header{0:%H-%M-%S}.txt", now)};

            std::ofstream out_file(time);
            if (!out_file) {std::cerr << "e"; return false;}

            
            out_file << raw_head << "\n";
            out_file.close();
            return true;

        }catch (const std::filesystem::filesystem_error& e){
            std::cerr << "Filesysten Error: " << e.what() << "\n";
        }
    }
    
    return false;
}
