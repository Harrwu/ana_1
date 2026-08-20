#include "url_parser.h"

UrlParts parseUrl_(const std::string_view raw_) {
    UrlParts res_{};
    
    size_t scheme_pos = raw_.find("://");
    if (scheme_pos == std::string_view::npos) return res_;

    std::string_view scheme_pass = raw_.substr(0, scheme_pos);
    res_.scheme = (std::string(scheme_pass));

    size_t host_pos = raw_.find("/", scheme_pos + 3);
    if (host_pos == std::string_view::npos) {
        std::string_view host_pass = raw_.substr(scheme_pos + 3);
        if (host_pass.find(".") == std::string_view::npos) return res_;
        else {
            res_.host = (std::string(host_pass));
            return res_;
        }
    }

    std::string_view host_pass = raw_.substr(scheme_pos + 3, host_pos - (scheme_pos + 3));
    std::string_view path_ = raw_.substr(host_pos);

    res_.host = (std::string(host_pass));
    res_.path = (std::string(path_));

    return res_;
}
