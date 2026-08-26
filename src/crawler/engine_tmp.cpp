#include "engine.h"
#include "builder.h"
#include "url_parser.h"
#include "html_parser.h"
#include "xml_parser.h"
#include "rss_parser.h"
#include "parser.h"
#include "yahoo.cpp"
#include <thread>
#include <chrono>
#include <memory>

void crawlEngine::run(){
    if (url_queue.empty()) {
        std::cout << "Nothing To Do\n";
        return;
    }

    UrlParts current_ = parseUrl_(url_queue.front());
    int port = (current_.scheme == "https") ? 443: 80;

    if (port == 443) sock_ = std::make_unique<TlsSocket_>();
    if (sock_->connect(current_.host, port) != Tcpsock_::rType::CONNECT_) {
        std::cout << "Connect Failed: \n";
        std::cout << url_queue.front() << "\n";
        return;
    }

    while (!url_queue.empty()) {
        std::string current_url = url_queue.front();
        url_queue.pop();

        UrlParts url_parts = parseUrl_(current_url);
        if (url_parts.host != current_.host) {
            port = (url_parts.host != current_.host)? 443: 80;
            if (port == 443) sock_ = std::make_unique<TlsSocket_>();
            else sock_ = std::make_unique<Tcpsock_>();

            if (sock_->connect(url_parts.host, port) != Tcpsock_::rType::CONNECT_) continue;
            current_ = url_parts;
        }
        
        constexpr std::string_view status = "close";
        auto buildReq = std::make_unique<ReqBuild>(url_parts.host, std::string(status));
        buildReq->SetMethod("GET");
        buildReq->SetPath(url_parts.path);
        buildReq->addHead("User-Agent", "Scrapper/1.1");

        std::string request_ = buildReq->build_();

        if (sock_->send(request_) == Tcpsock_::rType::CONNECT_) {
            std::string main_payload{};
            char buffer[4096] = {0};
            ssize_t bytes_read = 0;
            bool header = false, isChunked = false;
            size_t content_length = 0;

            while ((bytes_read = sock_->recv(buffer, sizeof(buffer))) > 0) {
                main_payload.append(buffer, bytes_read);
                std::fill(std::begin(buffer), std::end(buffer), '\0');

                if (!header) {
                    size_t boundary = main_payload.find("\r\n\r\n");
                    if (boundary != std::string::npos) {
                        size_t cl_pos = main_payload.find("Content-Length: ");
                        if (cl_pos != std::string::npos && cl_pos < boundary) {
                            size_t val_start = cl_pos + 16;
                            size_t val_end = main_payload.find("\r\n", val_start);
                            int contentLen = std::stoi(main_payload.substr(val_start, val_end - val_start));
                            content_length = contentLen;
                        }
                        else if (main_payload.find("Transfer-Encoding: chunked") != std::string::npos) isChunked = true;
                        else content_length = boundary + 4;
                    }
                }

                if (header) {
                    if (isChunked) 
                        if (main_payload.find("0\r\n\r\n") != std::string::npos) break;
                    else if (content_length > 0 && main_payload.length() >= content_length) break;
                }
            }

            if (!main_payload.empty()) std::cout << main_payload << "\n";

            /*
             TODO: Add A function that parses the text based on the exact text document type.
                    New file main_parse.cpp main_parse.h. Returns a string_view of the extracted text
                    
            */

        }

    }

    std::cout << "Empty Queue\n";
}
