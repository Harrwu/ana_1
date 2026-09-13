cmake_minimum_required(VERSION 3.10)
project(ana_1 VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_executable(core
    core/test_main.cpp 
    ai_engine/ai_server.cpp
    trading/broker_api.cpp
    network/tcp_socket.cpp 
    network/tls_socket.cpp
    http/builder.cpp 
    http/parser.cpp
    http/url_parser.cpp
    http/text_parser/html_parser.cpp
    http/text_parser/xml_parser.cpp
    http/text_parser/rss_parser.cpp
    crawler/engine.cpp
    crawler/cashtag_extractor.cpp
    crawler/state_manager.cpp
)

add_executable(crawl_client
    core/test_main.cpp 
    network/tcp_socket.cpp 
    http/builder.cpp 
    http/parser.cpp
    http/url_parser.cpp
    network/tls_socket.cpp
    http/text_parser/html_parser.cpp
    crawler/engine.cpp
    http/text_parser/xml_parser.cpp
    http/text_parser/rss_parser.cpp
    crawler/profile.h
    crawler/cashtag_extractor.cpp
    crawler/state_manager.cpp
)

add_executable(aiserv
    ai_engine/ai_server.cpp
    trading/broker_api.cpp
    network/tcp_socket.cpp
    network/tls_socket.cpp
    http/builder.cpp
)

target_include_directories(aiserv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}network
    ${CMAKE_CURRENT_SOURCE_DIR}http
    ${CMAKE_CURRENT_SOURCE_DIR}trading
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(aiserv PRIVATE OpenSSL::SSL OpenSSL::Crypto)

add_executable(brokercli
    trading/broker_cli.cpp
    trading/broker_api.cpp
    network/tcp_socket.cpp
    network/tls_socket.cpp
    http/builder.cpp
)

target_include_directories(brokercli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/network
    ${CMAKE_CURRENT_SOURCE_DIR}/http
    ${CMAKE_CURRENT_SOURCE_DIR}/trading
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(brokercli PRIVATE OpenSSL::SSL OpenSSL::Crypto)

target_include_directories(crawl_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/network
    ${CMAKE_CURRENT_SOURCE_DIR}/http
    ${CMAKE_CURRENT_SOURCE_DIR}/http/text_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/crawler
)

target_include_directories(core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/network
    ${CMAKE_CURRENT_SOURCE_DIR}/http
    ${CMAKE_CURRENT_SOURCE_DIR}/http/text_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/crawler
    ${CMAKE_CURRENT_SOURCE_DIR}/ai_engine
    ${CMAKE_CURRENT_SOURCE_DIR}/trading
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

#target_link_libraries(test_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(crawl_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(core PRIVATE OpenSSL::SSL OpenSSL::Crypto)