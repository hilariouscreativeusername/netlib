cmake_minimum_required (VERSION 3.0)
project(asio)

set(ASIO_INCLUDE_FILES asio/asio/include/asio.hpp)
source_group("asio" FILES ${ASIO_INCLUDE_FILES})

add_library(${PROJECT_NAME} INTERFACE ${ASIO_INCLUDE_FILES})
target_include_directories(${PROJECT_NAME} INTERFACE asio/asio/include)
