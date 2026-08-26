#include "xml_parser.h"

bool XmlParse_::extract(){
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
        if (end_pos == std::string_view::npos) {
            std::cout << "Error 1\n\n\n";
            break; 
        }
        
        std::string_view link = raw_body.substr(start_pos, end_pos - start_pos);

        if (idx == 1) {
            size_t child_start = link.find("<loc>");
            if (child_start == std::string_view::npos) {
                std::cout << "Error Start: " << std::string(link) << "\n";
            } else {
                child_start += 5;
                size_t child_end = link.find("</loc>", child_start);
                
                if (child_end == std::string_view::npos) {
                    std::cout << "Error End: " << std::string(link) << "\n";
                } else {
                    std::string_view child_link = link.substr(child_start, child_end - child_start);
                    size_t cdata_pos = child_link.find("CDATA");
                    
                    if (cdata_pos != std::string_view::npos) {
                        cdata_pos += 7;
                        size_t cdata_end = child_link.find("]]>", cdata_pos);
                        
                        if (cdata_end == std::string_view::npos) {
                            std::cout << "Error CDATA END: " << child_link << "\n";
                            glob_prev = end_pos + arr[idx + 2].length();
                            continue;
                        }
                        child_link = child_link.substr(cdata_pos, cdata_end - cdata_pos);
                    }
                    link_res.push_back(std::string(child_link));
                }
            }
        } else {
            size_t cdata_pos = link.find("CDATA");
            if (cdata_pos != std::string_view::npos){
                cdata_pos += 7;
                size_t cdata_end = link.find("]]>", cdata_pos);
                if (cdata_end == std::string_view::npos) {
                    std::cout << "Error CDATA END: " << link << "\n";
                    glob_prev = end_pos + arr[idx + 2].length();
                    continue;
                }
                link = link.substr(cdata_pos, cdata_end - cdata_pos);
            }
            link_res.push_back(std::string(link));
        }
        glob_prev = end_pos + arr[idx + 2].length();
    }   
    return true;
}
