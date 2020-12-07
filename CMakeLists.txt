cmake_minimum_required (VERSION 3.11)

project(wcp)

add_executable(wcp main.cpp)
target_link_libraries(wcp uring)
