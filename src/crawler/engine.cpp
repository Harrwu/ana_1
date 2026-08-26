#include "engine.h"
#include "builder.h"
#include "url_parser.h"
#include "html_parser.h"
#include "parser_base.h"
#include "xml_parser.h"
#include "rss_parser.h"
#include "parser.h"
#include "yahoo.cpp"
#include <thread>
#include <chrono> 
#include <memory>

std::string escapeJSON(const std::string& input) {
    std::string output;
    output.reserve(input.length());
    for (char c : input) {
        switch (c) {
            case '"':  output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n";  break;
            case '\r': output += "\\r";  break;
            case '\t': output += "\\t";  break;
            default:   output += c;
        }
    }
    return output;
}

void crawlEngine::run() {
    auto const now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
    std::string time {std::format("../_output/{0:%Y-%m-%d}/visited{0:%H-%M-%S}.txt", now)};

    std::ofstream links_file(time);
    //if (!links_file) return;

    if (url_queue.empty()) {
        std::cout << "Queue empty. Nothing to do.\n";
        return;
    }

    UrlParts current_conn = parseUrl_(url_queue.front());
    int port = (current_conn.scheme == "https") ? 443 : 80;
    
    if (port == 443) sock_ = std::make_unique<TlsSocket_>();
    else sock_ = std::make_unique<Tcpsock_>();
    
    if (sock_->connect(current_conn.host, port) != Tcpsock_::rType::CONNECT_) {
        std::cout << "Initial connection failed!\n";
        return;
    }

    YahooProfile profile;

    while (!url_queue.empty()) {
        std::string current_url = url_queue.front();
        url_queue.pop(); 

        UrlParts url_parts = parseUrl_(current_url);
        
        if (url_parts.host != current_conn.host) {
            port = (url_parts.scheme == "https") ? 443 : 80;
            if (port == 443) sock_ = std::make_unique<TlsSocket_>();
            else sock_ = std::make_unique<Tcpsock_>();
            
            if (sock_->connect(url_parts.host, port) != Tcpsock_::rType::CONNECT_) continue;
            current_conn = url_parts;
        }

        auto buildReq = std::make_unique<ReqBuild>(url_parts.host, "keep-alive");
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
                        if (main_payload.find("0\r\n\r\n") != std::string::npos) break; 
                    } else if (target_total_bytes > 0 && main_payload.length() >= target_total_bytes) {
                        break; 
                    }
                }
            }

            if (bytes_read >= 0) {
                Parse_ parse(main_payload);
                parse.extract();
                Response res = parse.getResponse();
                bool isParsed = false;
                
                std::unique_ptr<TextParser> main_parser = nullptr; 
                bool isXml = false; 
                std::vector<std::string> links;
                
                std::string pure_body = res.getBody();
                std::string content_type = res.getHeader("Content-Type");
                
                if (pure_body.find("<rss") != std::string::npos || pure_body.find("<feed") != std::string::npos) {
                    std::cout << "RSS\n"; 
                    isXml = true;
                    main_parser = std::make_unique<RssParse_>(pure_body);
                } else if (content_type.find("xml") != std::string::npos || 
                           pure_body.find("<?xml") != std::string::npos || 
                           pure_body.find("<urlset>") != std::string::npos) {
                    std::cout << "XML\n"; 
                    isXml = true;
                    main_parser = std::make_unique<XmlParse_>(pure_body);
                } else if (content_type.find("html") != std::string::npos || 
                           pure_body.find("<!doctype html>") != std::string::npos || 
                           pure_body.find("<!DOCTYPE html>") != std::string::npos ||
                           pure_body.find("<html") != std::string::npos) {
                    std::cout << "HTML\n";
                    main_parser = std::make_unique<HtmlParse_>(pure_body);
                    std::string article_text = profile.extractNewsText(pure_body);
                    isParsed = true;
                    
                    if (!article_text.empty()) {
                        std::string safe_article_text = escapeJSON(article_text);      
                        
                        auto ollama_sock = std::make_unique<Tcpsock_>();
                        if (ollama_sock->connect("127.0.0.1", 11434) == Tcpsock_::rType::CONNECT_) {
                            auto aiReq = std::make_unique<ReqBuild>("127.0.0.1", "close");
                            aiReq->SetMethod("POST"); 
                            aiReq->SetPath("/api/generate");

                            std::string json_payload = R"({
                                "model": "llama3.1",
                                "prompt": "Analyze this financial sentiment. Reply with EXACTLY ONE WORD: BULLISH, BEARISH, or NEUTRAL. Text: )" + safe_article_text + R"(",
                                "stream": false
                            })";

                            aiReq->SetBody(json_payload);
                            ollama_sock->send(aiReq->build_());

                            std::string ai_payload{};
                            char ai_buffer[4096] = {0};
                            ssize_t ai_bytes = 0;
                            
                            while ((ai_bytes = ollama_sock->recv(ai_buffer, sizeof(ai_buffer))) > 0) {
                                ai_payload.append(ai_buffer, ai_bytes);
                                std::fill(std::begin(ai_buffer), std::end(ai_buffer), '\0');
                            }
                            
                            size_t body_start = ai_payload.find("\r\n\r\n");
                            if (body_start != std::string::npos) {
                                std::string ai_body = ai_payload.substr(body_start + 4);
                                
                                size_t res_start = ai_body.find("\"response\":\"");
                                if (res_start != std::string::npos) {
                                    res_start += 12;
                                    size_t res_end = ai_body.find("\"", res_start);
                                    if (res_end != std::string::npos) {
                                        std::string final_sentiment = ai_body.substr(res_start, res_end - res_start);
                                        std::cout << "Final Sentiment: " << final_sentiment << "\n";
                                    }
                                }
                            }
                        }
                    }
                } 
        
                if (main_parser && !isParsed) {
                    main_parser->extract();
                    if (isXml) {
                        links = main_parser->getResLink(); 
                        if (!links.empty()) {
                            for (auto& x: links) {
                                if (profile.isArticleLink(x)) {
                                    if (visited_url.insert(x).second) { 
                                        addUrl(x); 
                                        links_file << x << "\n";
                                    }
                                }
                            }
                        } else {
                            std::cout << "No Link Found\n";
                        }
                    }
                } else if(main_parser) {
                    std::cout << "Unknown Content-Type\n";
                }
            }
        }
    }
    
    std::cout << "Queue empty. Crawler finished.\n";
}
