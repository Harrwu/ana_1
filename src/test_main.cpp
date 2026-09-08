#include "engine.h"
#include <memory>
#include <iostream>

int main() {
    auto crawl = std::make_unique<crawlEngine>();
    
    std::cout << "Starting Trading Crawler Engine...\n";
    
    // 1. Seed the queue with high-value RSS feeds
    // Yahoo Finance RSS
    crawl->addUrl("https://finance.yahoo.com/news/rss");
    
    // PR Newswire (Earnings/Mergers)
    crawl->addUrl("https://www.prnewswire.com/rss/news-releases-list.rss");
    
    // CNBC Top News RSS
    crawl->addUrl("https://www.cnbc.com/id/100003114/device/rss/rss.html");

    // 2. Start the continuous autonomous loop
    crawl->run();

    return 0;
}
