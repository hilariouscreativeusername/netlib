cmake_minimum_required(VERSION 3.0)
project(NetLibClientTest CXX)

set(NETLIB_CLIENT_TEST_FILES client_main.cpp)
source_group("src" FILES ${NETLIB_CLIENT_TEST_FILES})

add_executable(${PROJECT_NAME} ${NETLIB_CLIENT_TEST_FILES})

target_link_libraries(${PROJECT_NAME} PRIVATE NetLib)
