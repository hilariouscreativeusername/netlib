cmake_minimum_required(VERSION 3.0)
project(NetLibServerTest CXX)

set(NETLIB_SERVER_TEST_FILES server_main.cpp)
source_group("src" FILES ${NETLIB_SERVER_TEST_FILES})

add_executable(${PROJECT_NAME} ${NETLIB_SERVER_TEST_FILES})

target_link_libraries(${PROJECT_NAME} PRIVATE NetLib)
