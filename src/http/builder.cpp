#include "builder.h"

ReqBuild::ReqBuild(const std::string& tar_host, const std::string& connection): host(tar_host) {
    headers["Host"] = host;
    headers["Connection"] = connection;
}

const std::string ReqBuild::build_() {
    std::stringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    for (const auto& x: headers) 
        req << x.first << ": " << x.second << "\r\n";
    req << "\r\n";
    
    if (!body.empty()) req << body;

    return req.str();
}

void ReqBuild::clear_() {
    method = "GET";
    path = "/";
    body.clear();
    headers.clear();
    headers["Host"] = host;
    headers["Connection"] = "close";
}
