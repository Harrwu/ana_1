#include "cashtag_extractor.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>

namespace {

std::string toLowerCopy(const std::string& input) {
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string toUpperCopy(const std::string& input) {
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return result;
}

std::string decodeDollarEntities(const std::string& text) {
    std::string result = text;

    auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.length(), to);
            pos += to.length();
        }
    };

    replaceAll(result, "&#36;", "$");
    replaceAll(result, "&#36", "$");
    replaceAll(result, "&#x24;", "$");
    replaceAll(result, "&#X24;", "$");
    replaceAll(result, "&#x24", "$");
    replaceAll(result, "&#X24", "$");
    replaceAll(result, "&dollar;", "$");

    return result;
}

bool isWordCharacter(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

bool containsWholePhrase(const std::string& lowerText, const std::string& lowerPhrase) {
    if (lowerPhrase.empty()) return false;

    size_t searchPos = 0;
    while (true) {
        size_t pos = lowerText.find(lowerPhrase, searchPos);
        if (pos == std::string::npos) return false;

        bool validBefore = (pos == 0) || !isWordCharacter(lowerText[pos - 1]);
        size_t endPos = pos + lowerPhrase.length();
        bool validAfter = (endPos >= lowerText.length()) || !isWordCharacter(lowerText[endPos]);

        if (validBefore && validAfter) return true;
        searchPos = pos + 1;
    }
}

void trimInPlace(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    s = s.substr(start);
}

// Reads data/companies.csv (name,ticker per line). Tries a couple of
// reasonable working-directory guesses since the binary might be run from
// the repo root, from build/, etc.
std::vector<std::pair<std::string, std::string>> loadCompanyMapFromDisk() {
    std::vector<std::pair<std::string, std::string>> companies;

    const char* candidatePaths[] = {
        "data/companies.csv",
        "../data/companies.csv",
        "companies.csv",
    };

    std::ifstream file;
    std::string usedPath;
    for (const char* path : candidatePaths) {
        file.open(path);
        if (file.is_open()) {
            usedPath = path;
            break;
        }
    }

    if (!file.is_open()) {
        std::cout << "[CASHTAGS] Warning: could not find data/companies.csv "
                     "(looked in data/, ../data/, and .). Company-name "
                     "matching is disabled -- $TICKER cashtags still work.\n";
        return companies;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF safety
        if (line.empty() || line[0] == '#') continue;

        size_t comma = line.find(',');
        if (comma == std::string::npos) continue;

        std::string name = line.substr(0, comma);
        std::string ticker = line.substr(comma + 1);
        trimInPlace(name);
        trimInPlace(ticker);

        if (!name.empty() && !ticker.empty()) {
            companies.emplace_back(toLowerCopy(name), toUpperCopy(ticker));
        }
    }

    std::cout << "[CASHTAGS] Loaded " << companies.size()
              << " company names from " << usedPath << "\n";
    return companies;
}

}  // namespace

const std::vector<std::pair<std::string, std::string>>& companyMap() {
    static const std::vector<std::pair<std::string, std::string>> companies = loadCompanyMapFromDisk();
    return companies;
}

std::string extractCashtags(const std::string& text) {
    const std::string decodedText = decodeDollarEntities(text);
    std::set<std::string> detectedTickers;

    static const std::regex cashtagRegex(
        R"(\$([A-Za-z]{1,5}(?:\.[A-Za-z]{1,2})?)(?![A-Za-z0-9_]))"
    );

    for (std::sregex_iterator it(decodedText.begin(), decodedText.end(), cashtagRegex);
         it != std::sregex_iterator(); ++it) {
        detectedTickers.insert(toUpperCopy((*it)[1].str()));
    }

    const std::string lowerText = toLowerCopy(decodedText);

    for (const auto& [name, ticker] : companyMap()) {
        if (!ticker.empty() && containsWholePhrase(lowerText, name)) {
            detectedTickers.insert(ticker);
        }
    }

    if (detectedTickers.empty()) return "MACRO";

    std::string result;
    for (const auto& ticker : detectedTickers) {
        if (!result.empty()) result += ",";
        result += ticker;
    }
    return result;
}
