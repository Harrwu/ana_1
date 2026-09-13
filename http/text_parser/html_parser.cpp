#include "html_parser.h"

void HtmlParse_::mainLnk(std::ofstream& link_file) const{
    size_t search_offset {0}; 
    while (true) {
        size_t href_pos = raw_body.find("href=\"", search_offset);
        if (href_pos == std::string_view::npos) {
            break; 
        }

        size_t url_start = href_pos + 6;

        size_t url_end = raw_body.find("\"", url_start);
        if (url_end == std::string_view::npos) {
            break; 
        }
        std::string_view extracted_url = raw_body.substr(url_start, url_end - url_start);

        std::cout << extracted_url << "\n";
        link_file << extracted_url << "\n";

        search_offset = url_end + 1;
    }
}

const std::string HtmlParse_::rePull(std::string_view extra_) const {
    std::string clean_text;
    clean_text.reserve(extra_.length()); 
    
    bool inside_tag = false;

    for (char c : extra_) {
        if (c == '<') {
            inside_tag = true;
        } else if (c == '>') {
            inside_tag = false;
        } else if (!inside_tag) {
            clean_text.push_back(c);
        }
    }
    return clean_text;
}

bool HtmlParse_::extract(){
    bool eof {false};
    size_t glob_prev {0};

    std::cout << "Extracting\n";
        
    const auto now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
    std::string time = std::format("../_output/{0:%Y-%m-%d}/parse{0:%H-%M-%S}.txt", now);
    //std::string link_ = std::format("../_output/{0:%Y-%m-%d}/linksDebug{0:%H-%M-%S}.txt", now);
    std::ofstream out_file(time);
    if (!out_file) {std::cout << "Error Opening File Out Main\n"; return false; }
    //if (!link_file) {std::cout << "Error Opening File Link\n"; return false;}

    //mainLnk(link_file);

    if (isFile) {
        while (!eof) {
            std::set<std::pair<size_t, int>> curr_vec{};
            int npos_count {0};

            for (int i = 0; i < 7; i++) {
                size_t pass = raw_body.find(arr[i], glob_prev);
                
                if (pass == std::string_view::npos) {
                    npos_count++;
                } else {
                    curr_vec.insert({pass, i});
                }
            }

            if (npos_count == 7 || curr_vec.empty()) {
                eof = true; 
                break;
            }

            auto x = *curr_vec.begin(); 
            size_t tag_start = x.first;
            int tag_id = x.second;

            size_t content_start = raw_body.find(">", tag_start);
            if (content_start == std::string_view::npos) {
                glob_prev = tag_start + arr[tag_id].length();
                continue;
            }
            content_start += 1;

            size_t closing_pos = raw_body.find(arr[tag_id + 7], content_start);
            if (closing_pos == std::string_view::npos) {
                glob_prev = content_start;
                continue;
            }

            size_t text_len = closing_pos - content_start;
            std::string_view raw_extracted = raw_body.substr(content_start, text_len);
            //if (tag_id == 7) 
                //link_file << std::string(raw_extracted) << "\n";
            

            std::string english = rePull(raw_extracted);

            out_file << std::string(english) << "\n";
            //std::cout << std::string(english) << "\n";
            glob_prev = closing_pos + arr[tag_id + 7].length();
        } 
        //link_file.close();
        out_file.close();
        return true;
    }
    return false;
}
