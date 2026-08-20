#include "xml_parser.h"

bool XmlParse_::extract(){
    const auto now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
    std::string link_ = std::format("../_output/{0:%Y-%m-%d}/linksXml{0:%H-%M-%S}.txt", now);
    std::ofstream link_file(link_);
    if (!link_file) {std::cerr << "Error Opening File XML\n"; return false;}

    bool eof = false;
    size_t glob_prev {0};

    while (!eof) {
        std::set<std::pair<size_t, int>> curr_vec{};
        for (int i = 0; i < 2; i ++) {
            size_t pass_pos = raw_body.find(arr[i], glob_prev);
            if (pass_pos != std::string_view::npos) curr_vec.insert({pass_pos, i});
        }

        if (curr_vec.empty()) {eof = true; break;}

        auto x = *curr_vec.begin();
        size_t start_pos = x.first;
        int idx = x.second;

        start_pos += arr[x.second].length();

        size_t end_pos = raw_body.find(arr[idx + 2], start_pos);
        if (end_pos == std::string_view::npos) {std::cout << "Error 1\n\n\n";}
        std::string_view link = raw_body.substr(start_pos, end_pos - start_pos);
        std::cout << std::string(link) << "\n";

        if (idx == 1) {
            size_t child_start = link.find("<loc>");
            if (child_start == std::string_view::npos) {
                std::cout << "Error Start: " << std::string(link) << "\n";
            }

            child_start += 5;
            size_t child_end = link.find("</loc>", child_start);
            if (child_end == std::string_view::npos) {
                std::cout << "Error End: " << std::string(link) << "\n";
            }

            std::string_view child_link = link.substr(child_start, child_end - child_start);
            std::cout << std::string(child_link);
        }

        glob_prev = end_pos + arr[idx + 2].length();
    }

    return true;
}
