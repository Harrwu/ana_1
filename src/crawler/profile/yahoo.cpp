//#pragma once
#include "../profile.h"
#include <string>

class YahooProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        // 1. Must be a Yahoo Finance link
        if (url.find("finance.yahoo.com") == std::string::npos) {
            return false; 
        }

        // 2. The Time Machine Filter: Ignore old archives
        if (url.find("201") != std::string::npos || // Blocks 2010-2019
            url.find("2020") != std::string::npos ||
            url.find("2021") != std::string::npos ||
            url.find("2022") != std::string::npos ||
            url.find("2023") != std::string::npos) {
            
            return false; 
        }

        return true; 
    }

    std::string extractNewsText(const std::string& raw_html) const override {
        std::string article_text = "";
        size_t search_pos = 0;
        
        // Loop through the HTML and extract text inside <p> tags
        while (true) {
            size_t p_start = raw_html.find("<p", search_pos);
            if (p_start == std::string::npos) break;
            
            size_t p_end = raw_html.find("</p>", p_start);
            if (p_end == std::string::npos) break;
            
            // Jump past the closing bracket of the <p class="..."> tag
            size_t text_start = raw_html.find(">", p_start);
            if (text_start == std::string::npos || text_start > p_end) {
                search_pos = p_end + 4;
                continue;
            }
            text_start += 1; 
            
            std::string paragraph = raw_html.substr(text_start, p_end - text_start);
            
            // Simple filter: only keep clean text without nested HTML links/scripts
            if (paragraph.find("<") == std::string::npos && paragraph.length() > 20) {
                article_text += paragraph + "\n\n";
            }
            
            search_pos = p_end + 4;
        }
        
        return article_text;
    }
};
