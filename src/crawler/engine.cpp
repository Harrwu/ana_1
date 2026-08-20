#include "engine.h"
#include "builder.h"
#include "url_parser.h"
#include "html_parser.h"
#include "xml_parser.h"
#include "parser.h"

void crawlEngine::run() {
    while (!url_queue.empty()) {
        UrlParts url_parts = parseUrl_(url_queue.front());
        url_queue.pop();
        std::string addr = url_parts.host;
        const int port = (url_parts.scheme == "https")? 443: 80;
        if (port == 443) sock_ = std::make_unique<TlsSocket_>();
        
        auto status = sock_->connect(addr, port);
        auto buildReq = std::make_unique<ReqBuild>(addr, "keep-alive");

        if (status == Tcpsock_::rType::CONNECT_) {
            buildReq->SetMethod("GET");
            buildReq->SetPath(url_parts.path);
            buildReq->addHead("User-Agent", "Scrapper/1.0");

            std::string request_ = buildReq->build_();
            if (sock_->send(request_) == Tcpsock_::rType::CONNECT_) {
                std::string main_payload{};
                char buffer[4096] = {0};
                ssize_t bytes_read = 0;

                bool headers_found = false;
                bool is_chunked = false;
                size_t target_total_bytes = 0;
                std::cout << "Sending Request\n";

                while ((bytes_read = sock_->recv(buffer, sizeof(buffer))) > 0) { 
                    main_payload.append(buffer, bytes_read);
                    std::fill(std::begin(buffer), std::end(buffer), '\0');

                    if (!headers_found) {
                        size_t boundary = main_payload.find("\r\n\r\n");
                        
                        if (boundary != std::string::npos) {
                            headers_found = true;
                            
                            
                            size_t cl_pos = main_payload.find("Content-Length: ");
                            if (cl_pos != std::string::npos && cl_pos < boundary) {
                                size_t val_start = cl_pos + 16;
                                size_t val_end = main_payload.find("\r\n", val_start);
                                int content_len = std::stoi(main_payload.substr(val_start, val_end - val_start));
                                
                                target_total_bytes = boundary + 4 + content_len;
                                
                            } else if (main_payload.find("Transfer-Encoding: chunked") != std::string::npos) {
                                is_chunked = true;
                                
                            } else {
                                target_total_bytes = boundary + 4;
                            }
                        }
                    }

                    if (headers_found) {
                        if (is_chunked) {
                            if (main_payload.find("0\r\n\r\n") != std::string::npos) {
                                std::cout << "Chunked Terminator found \n\n";
                                break; 
                            }
                        } else if (target_total_bytes > 0 && main_payload.length() >= target_total_bytes) {
                            std::cout << "EOF using Content Length\n\n";
                            break; 
                        }
                    }
                }

                if (bytes_read < 0) {
                    std::cout << "Error Receiving Data\n";
                } else {
                    Parse_ parse(main_payload);
                    parse.extract();
                    parse.writeBody();
                    parse.writeHead();

                    //HtmlParse_ html(main_payload);
                    //html.extract();
                    //
                    XmlParse_ xml(main_payload);
                    xml.extract();
                    
                }    
        }
    }
    std::cout << "Nothing Left To do\n";
}}
