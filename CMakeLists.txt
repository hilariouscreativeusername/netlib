cmake_minimum_required(VERSION 3.0)
project(NetLib CXX)

set(NETLIB_INCLUDE_FILES include/netlib/netlib.hpp)
source_group("netlib" FILES ${NETLIB_INCLUDE_FILES})

add_library(${PROJECT_NAME} INTERFACE ${NETLIB_INCLUDE_FILES})
target_include_directories(${PROJECT_NAME} INTERFACE include)
target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_17)

add_subdirectory(test/server)
add_subdirectory(test/client)

add_subdirectory(depend/asio)
target_link_libraries(${PROJECT_NAME} INTERFACE asio)
