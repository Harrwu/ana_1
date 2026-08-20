#pragma once
#include <iostream>
#include <map>
#include <string>

class Response {
private:
    
    int status_code {-1};
    std::string status_line {};
    std::map<std::string, std::string> headers{};
    std::string body{};

public:
    
    Response() = default;

    void setCode(const int pass_code) {status_code = pass_code;}
    void setStat(const std::string& pass_stat) {status_line = pass_stat;}
    void addHead(const std::string& key, const std::string& val) {headers[key] = val;}
    void setBody(const std::string& pass_body) {body = pass_body;}

    int getCode() const { return status_code; }
    const std::map<std::string, std::string>& getMap() const{return headers;}
    std::string getBody() const { return body; }
    std::string getHeader(const std::string& key) { 
        if (headers.find(key) != headers.end()) {
            return headers[key];
        }
        return "";
    }

};
