#include "url_parser.h"

UrlParts parseUrl_(const std::string_view raw_) {
    UrlParts res_{};
    
    size_t scheme_pos = raw_.find("://");
    if (scheme_pos == std::string_view::npos) return res_;

    res_.scheme = std::string(raw_.substr(0, scheme_pos));

    size_t host_start = scheme_pos + 3;
    
    size_t path_pos = raw_.find("/", host_start);
    
    std::string_view host_view;
    
    if (path_pos == std::string_view::npos) {
        host_view = raw_.substr(host_start);
        res_.path = "/";
    } else {
        host_view = raw_.substr(host_start, path_pos - host_start);
        res_.path = std::string(raw_.substr(path_pos));
    }

    res_.host = std::string(host_view);
    
    /*if (res_.host.find("www.") != 0) { 
        res_.host = "www." + res_.host;
    }*/

    return res_;
}
