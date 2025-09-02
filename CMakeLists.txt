cmake_minimum_required(VERSION 3.16)

project(coroutine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(coroutine main.cpp)
target_compile_options(coroutine PRIVATE -Wall -Wextra -Wpedantic)
include(GNUInstallDirs)
install(TARGETS coroutine
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
