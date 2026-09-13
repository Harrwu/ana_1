#include "rss_parser.h"

bool RssParse_::extract(){
    bool eof = false;
    size_t glob_prev {0};

    while (!eof) {
        std::set<std::pair<size_t, int>> curr_vec{};
        for (int i = 0; i < 1; i ++) {
            size_t pass_pos = raw_body.find(arr[i], glob_prev);
            if (pass_pos != std::string_view::npos) curr_vec.insert({pass_pos, i});
        }

        if (curr_vec.empty()) {eof = true; break;}

        auto x = *curr_vec.begin();
        size_t start_pos = x.first;
        int idx = x.second;

        start_pos += arr[x.second].length();

        size_t end_pos = raw_body.find(arr[idx + 1], start_pos);
        if (end_pos == std::string_view::npos) {
            std::cout << "Error 1\n\n\n";
            break; 
        }
        
        std::string_view link = raw_body.substr(start_pos, end_pos - start_pos);
        //std::cout << link << "\n";
        link_res.push_back(std::string(link));
        glob_prev = end_pos + arr[idx + 1].length();
    }   
    return true;
}

