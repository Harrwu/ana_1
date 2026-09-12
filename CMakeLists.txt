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
    src/crawler/profile.h
    src/crawler/cashtag_extractor.cpp
    src/crawler/state_manager.cpp
)

add_executable(aiserv
    src/ai_engine/ai_server.cpp
    src/trading/broker_api.cpp
    src/network/tcp_socket.cpp
    src/network/tls_socket.cpp
    src/http/builder.cpp
)

target_include_directories(aiserv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/trading
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(aiserv PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# Broker client. Kept as a separate target so the crawler and the trade
# executor can be run (and killed) independently -- a crash in order
# handling should never take the data pipeline down with it.
add_executable(brokercli
    src/trading/broker_cli.cpp
    src/trading/broker_api.cpp
    src/network/tcp_socket.cpp
    src/network/tls_socket.cpp
    src/http/builder.cpp
)

target_include_directories(brokercli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/trading
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(brokercli PRIVATE OpenSSL::SSL OpenSSL::Crypto)

target_include_directories(main_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/text_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/crawler
)
#[[
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
    src/crawler/profile.h
)

target_include_directories(test_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http/text_parser
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/crawler
)
]]


#target_link_libraries(test_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(main_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
