#include "tcp_socket.h"
#include "tls_socket.h"
#include "builder.h"
#include "parser.h"
#include "html_parser.h"
#include "url_parser.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

const int size_char_(const char* payload_pass) {
    int res {0};
    for (const char* ptr {payload_pass}; *ptr != '\0'; ptr ++) res ++;
    return res;
}

int main() {
    auto sock_ = std::make_unique<Tcpsock_>();

    std::string url {};
    std::cin >> url;

    UrlParts url_part = parseUrl_(url);

    std::string addr {url_part.host};
    static int portno = 80;
    if (url_part.scheme == "https") {portno = 443; sock_ = std::make_unique<TlsSocket_>();}

    std::cout << "Connecting to " << addr << ":" << portno << "...\n";
    
    auto status = sock_->connect(addr, portno);
    auto buildReq {std::make_unique<ReqBuild>(addr)};

    std::string payload_main{};

    if (status == Tcpsock_::rType::CONNECT_) {
        std::cout << "Connected! Sending request...\n";

        buildReq->SetMethod("GET");
        buildReq->SetPath(url_part.path);

        buildReq->addHead("User-Agent", "Scraper/1.0");

        std::string _request = buildReq->build_();
        std::cout << "\nReq: " << _request << "\nWaiting for response... \n";
        
        if (sock_->send(_request) == Tcpsock_::rType::CONNECT_) {
            std::string main_buff{};
            char buffer[4096] = {0};
            ssize_t bytes_read{0};

            while ((bytes_read = sock_->recv(buffer, sizeof(buffer))) > 0) {
                main_buff.append(buffer, bytes_read);
                std::fill(std::begin(buffer), std::end(buffer), '\0');
            }

            if (bytes_read < 0) {
                std::cout << "Error receiving data from server.\n";
            } else {
                std::cout << "Server Response Complete!\n\n";
                Parse_ parse(main_buff);
    
                Response res = parse.parse();
                parse.debug();

                if (parse.writeHead() && parse.writeBody()) {
                    std::cout << "\nSuccess on writing to file\n";
                    std::cout << "\n\n\nParsing body\n\n\n";

                    HtmlParse_ body_parse_ (res.getBody());
                    body_parse_.mainTxt();    
                }
                else std::cout << "\nError Writing\n";
            }
        }else std::cout << "Error sending to server\n";
    }else  std::cout << "Error connecting to server. Error Code: " << static_cast<int>(status) << "\n";
    
    return 0;
}

