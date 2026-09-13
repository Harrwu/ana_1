#pragma once
#include <iostream>
#include <map>
#include <sstream>
#include <string>

class ReqBuild {
private:
    std::string method{"GET"};
    std::string path{"/"};
    std::string host;
    std::map<std::string, std::string> headers;
    std::string body;

public:
    ReqBuild(const std::string& tar_host, const std::string& connection);

    void inline SetMethod(const std::string& new_method) { method = new_method; }
    void inline SetPath(const std::string& new_path) { path = new_path; }
    void inline addHead(const std::string& key, const std::string& value) { headers[key] = value; }
    
    void inline SetBody(const std::string& content, const std::string& content_type = "application/json") {
        body = content;
        headers["Content-Length"] = std::to_string(body.length());
        headers["Content-Type"] = content_type;
    }
    
    void clear_();
    const std::string build_();
};
