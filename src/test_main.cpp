#include "engine.h"
#include <memory>

int main() {
    auto crawl = std::make_unique<crawlEngine>();
    std::string url; std::cin >> url;
    crawl->addUrl(url);
    crawl->run();

    return 0;
}
