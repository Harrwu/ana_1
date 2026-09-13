#include "tls_socket.h"

TlsSocket_::TlsSocket_ (){
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    const SSL_METHOD* method = TLS_client_method();
    ctx = SSL_CTX_new(method);

    if (!ctx) {
        std::cerr << "Unable to create SSL context\n";
    }
}

TlsSocket_::~TlsSocket_() {
    Close();
    if (ctx) {
        SSL_CTX_free(ctx);
    }
}

Tcpsock_::rType TlsSocket_::connect(const std::string& hostname, int portno) {
    rType base_status = Tcpsock_::connect(hostname, portno);
    if (base_status != rType::CONNECT_) return base_status;

    ssl = SSL_new(ctx);
    if (!ssl) return rType::CONNECT_E;

    SSL_set_tlsext_host_name(ssl, hostname.c_str());
    SSL_set_fd(ssl, this->sockfd);

    if (SSL_connect(ssl) != 1) {
        std::cerr << "TLS Handshake failed\n";
        return rType::CONNECT_E;
    }

    return rType::CONNECT_;
}

Tcpsock_::rType TlsSocket_::send(const std::string& packet) {
    if (isConnected && ssl) {
        int n = SSL_write(ssl, packet.c_str(), packet.length());
        if (n <= 0) return rType::CONNECT_E;
        return rType::CONNECT_;
    }
    return rType::CONNECT_E;
}

ssize_t TlsSocket_::recv(char* buffer, size_t max_size) {
    if (isConnected && ssl) {
        int n = SSL_read(ssl, buffer, max_size);
        return n;
    }
    return -1;
}

void TlsSocket_::Close() {
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ssl = nullptr;
    }
   
    Tcpsock_::Close(); 
}
