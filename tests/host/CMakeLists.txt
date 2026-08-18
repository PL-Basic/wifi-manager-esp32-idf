cmake_minimum_required(VERSION 3.16)

project(wifi_manager_host_tests C)

enable_testing()

add_executable(
    tls_sni_parser_test
    tls_sni_parser_test.c
    ../../components/tls_sni_parser/tls_sni_parser.c
)

target_include_directories(
    tls_sni_parser_test
    PRIVATE ../../components/tls_sni_parser/include
)

if(MSVC)
    target_compile_options(tls_sni_parser_test PRIVATE /W4)
else()
    target_compile_options(
        tls_sni_parser_test
        PRIVATE -Wall -Wextra -Wpedantic
    )
endif()

add_test(NAME tls_sni_parser COMMAND tls_sni_parser_test)
