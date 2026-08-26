cmake_minimum_required(VERSION 3.10)
project(ana_1 VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_executable(main_client 
    src/test_main.cpp 
    src/network/tcp_socket.cpp 
    src/http/builder.cpp 
    src/http/parser.cpp
    src/http/url_parser.cpp
    src/network/tls_socket.cpp
    src/http/text_parser/html_parser.cpp
    src/crawler/engine.cpp
    src/http/text_parser/xml_parser.cpp
    src/http/text_parser/rss_parser.cpp
    src/crawler/profile/yahoo.cpp
)

target_include_directories(main_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/text_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/crawler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/crawler/profile
)

add_executable(test_client
    src/test2_main.cpp 
    src/network/tcp_socket.cpp 
    src/http/builder.cpp 
    src/http/parser.cpp
    src/http/url_parser.cpp
    src/network/tls_socket.cpp
    src/http/text_parser/html_parser.cpp
    src/crawler/engine_tmp.cpp
    src/http/text_parser/xml_parser.cpp
    src/http/text_parser/rss_parser.cpp
    src/crawler/profile/yahoo.cpp
)

target_include_directories(test_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/text_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/crawler
    ${CMAKE_CURRENT_SOURCE_DIR}/src/crawler/profile
)



target_link_libraries(test_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(main_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
