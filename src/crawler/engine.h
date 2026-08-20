#pragma once
#include "tls_socket.h"
#include "tcp_socket.h"
#include <queue>
#include <unordered_set>
#include <memory>
#include <iostream>
#include <string>

class crawlEngine{
private:
    std::unordered_set<std::string> visited_url {};
    std::queue<std::string> url_queue {};
    std::unique_ptr<Tcpsock_> sock_{};
    bool isHtml = false;
    
public:
    
    crawlEngine() = default;
    void addUrl(const std::string& url) {url_queue.push(url);}
    void run();

};
