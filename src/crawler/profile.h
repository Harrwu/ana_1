#pragma once
#include <string>

// Abstract Base Class
class SiteProfile {
public:
    virtual ~SiteProfile() = default;
    virtual bool isArticleLink(const std::string& url) const = 0;
    virtual std::string extractNewsText(const std::string& raw_html) const = 0;
};

// Inline helper so it doesn't trigger multiple-definition linker errors
inline std::string genericParagraphExtractor(const std::string& raw_html) {
    std::string article_text = "";
    size_t search_pos = 0;
    while (true) {
        size_t p_start = raw_html.find("<p", search_pos);
        if (p_start == std::string::npos) break;
        size_t p_end = raw_html.find("</p>", p_start);
        if (p_end == std::string::npos) break;
        
        size_t text_start = raw_html.find(">", p_start);
        if (text_start == std::string::npos || text_start > p_end) {
            search_pos = p_end + 4;
            continue;
        }
        text_start += 1; 
        
        std::string raw_paragraph = raw_html.substr(text_start, p_end - text_start);
        
        // NEW: Fast Inner-Tag Stripper
        std::string clean_paragraph = "";
        bool in_tag = false;
        for (char c : raw_paragraph) {
            if (c == '<') in_tag = true;
            else if (c == '>') in_tag = false;
            else if (!in_tag) clean_paragraph += c;
        }
        
        if (clean_paragraph.length() > 20) {
            article_text += clean_paragraph + "\n\n";
        }
        search_pos = p_end + 4;
    }
    return article_text;
}

// 1. Yahoo Finance
class YahooProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("finance.yahoo.com/news") != std::string::npos || 
               url.find("finance.yahoo.com/m") != std::string::npos;
    }
    std::string extractNewsText(const std::string& raw_html) const override {
        return genericParagraphExtractor(raw_html);
    }
};

// 2. CNBC
class CnbcProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.cnbc.com/20") != std::string::npos && 
               url.find("/video/") == std::string::npos;
    }
    std::string extractNewsText(const std::string& raw_html) const override {
        return genericParagraphExtractor(raw_html);
    }
};

// 3. PR Newswire
class PRNewswireProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.prnewswire.com/news-releases") != std::string::npos;
    }
    std::string extractNewsText(const std::string& raw_html) const override {
        return genericParagraphExtractor(raw_html);
    }
};

// 4. Benzinga
class BenzingaProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.benzinga.com/markets") != std::string::npos ||
               url.find("www.benzinga.com/news") != std::string::npos;
    }
    std::string extractNewsText(const std::string& raw_html) const override {
        return genericParagraphExtractor(raw_html);
    }
};

// 5. Wall Street Journal
class WsjProfile : public SiteProfile {
public:
    bool isArticleLink(const std::string& url) const override {
        return url.find("www.wsj.com/articles") != std::string::npos ||
               url.find("www.wsj.com/finance") != std::string::npos;
    }
    std::string extractNewsText(const std::string& raw_html) const override {
        return genericParagraphExtractor(raw_html);
    }
};
