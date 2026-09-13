#pragma once
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cerrno>
#include <cstring>

#include <iostream>
#include <memory>
#include <string>

class Tcpsock_ {
protected:
    int sockfd;
    sockaddr_in serv_addr{};
    bool isConnected {false};

public:
    enum class rType {
        SOCKET_E,
        SERV_E,
        CONNECT_E,
        CONNECT_,
        HOSTN_E
    };
    
    virtual ~Tcpsock_();

    virtual Tcpsock_::rType connect(const std::string& hostname, int portno);
    virtual void Close();

    virtual inline Tcpsock_::rType send(const std::string& packet) {
        if (isConnected) {
            ssize_t n = ::send(this->sockfd, packet.c_str(), packet.length(), MSG_NOSIGNAL);
            if (n == 0) return rType::SERV_E;
            if (n < 0) return rType::SOCKET_E;
            return rType::CONNECT_; 
        }
        return rType::CONNECT_E;
    }

    virtual inline ssize_t recv(char* buffer, size_t max_size) {
        if (isConnected) {
            ssize_t n = ::recv(this->sockfd, buffer, max_size, MSG_NOSIGNAL);
            return n;
        }
        return -1;
    }
    
};
