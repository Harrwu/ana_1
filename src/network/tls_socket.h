#pragma once
#include <iostream>
#include <memory>
#include "tcp_socket.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

class TlsSocket_: public Tcpsock_ {
private:
    SSL_CTX* ctx {};
    SSL* ssl {};

public:
    
    TlsSocket_();
    virtual ~TlsSocket_() override;

    Tcpsock_::rType connect(const std::string& hostname, int portno) override;
    void Close() override;
    
    Tcpsock_::rType send(const std::string& packet) override;
    ssize_t recv(char* buffer, size_t max_size) override;    
};
