#pragma once
#include <string>
#include <utility>
#include <vector>

// Returns the ticker/company-name lookup table, loaded once from
// data/companies.csv on first use and cached for the rest of the program's
// life. Format of each CSV line: "company name,TICKER" ('#' = comment line).
const std::vector<std::pair<std::string, std::string>>& companyMap();

// Extracts cashtags ($AAPL style) and known company names from `text`.
// Returns a comma-separated, de-duplicated, uppercase ticker list, or
// "MACRO" if nothing was found.
std::string extractCashtags(const std::string& text);