cmake_minimum_required(VERSION 3.16)
project(sqlite)

set(CMAKE_CXX_STANDARD 17)

add_library(sqlite OBJECT
		sqlite3.c
)

target_include_directories(sqlite PUBLIC .)