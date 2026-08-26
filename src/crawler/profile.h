#pragma once
#include <string>

class SiteProfile {
public:
    virtual ~SiteProfile() = default;
    
    virtual bool isArticleLink(const std::string& url) const = 0;
    
    virtual std::string extractNewsText(const std::string& raw_html) const = 0;
};
