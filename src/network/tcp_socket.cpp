#include "tcp_socket.h"

Tcpsock_::~Tcpsock_() {
    Tcpsock_::Close();
}

void Tcpsock_::Close(){
    if (isConnected && sockfd >= 0) {
        ::close(sockfd);
        isConnected = false;
        sockfd = -1;
    }
}

Tcpsock_::rType Tcpsock_::connect(const std::string& hostname, int portno) {
    struct addrinfo hints{}, *res;
    
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    std::string port_str = std::to_string(portno);
    
    if (getaddrinfo(hostname.c_str(), port_str.c_str(), &hints, &res) != 0) {
        return rType::HOSTN_E;
    }
    
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue; 
        
        if (::connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) {
            isConnected = true; 
            break;
        }
        
        ::close(sockfd);
        sockfd = -1;
    }
    
    freeaddrinfo(res); 
    
    if (!isConnected) {
        std::cerr << "Connect failed: " << strerror(errno) << "\n";
        return rType::SERV_E;
    }
    
    return rType::CONNECT_;
}
