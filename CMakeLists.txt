cmake_minimum_required(VERSION 3.10)
project(ninjireplay)

set(CMAKE_CXX_STANDARD 20)

add_compile_options(-Wno-ambiguous-reversed-operator)
add_compile_options(-Wno-deprecated-declarations)
add_compile_options(-Wno-unused-parameter)
add_compile_options(-Wno-extern-initializer)

if(CMAKE_BUILD_TYPE EQUAL "DEBUG")
	add_link_options(-g -O0)
endif()

# Skia for rendering QR codes and programs
set(SKIA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/skia)
set(SKIA_BUILD_TYPE out/Static)
set(SKIA_CMAKE_DIR ${SKIA_DIR}/${SKIA_BUILD_TYPE})
set(SKIA_LINK ${SKIA_CMAKE_DIR})
execute_process(
	COMMAND python tools/git-sync-deps
	WORKING_DIRECTORY ${SKIA_DIR})
execute_process(
	COMMAND bin/gn gen ${SKIA_BUILD_TYPE} "--args= is_official_build=true clang_win=\"C:/Program Files/LLVM\" extra_cflags=[\"/MDd\"] skia_use_system_libjpeg_turbo=false skia_use_system_libwebp=false skia_use_expat=false skia_use_system_zlib=false skia_use_system_libpng=false skia_use_system_harfbuzz=false skia_use_system_icu=false"
	WORKING_DIRECTORY ${SKIA_DIR})
execute_process(COMMAND ninja WORKING_DIRECTORY ${SKIA_CMAKE_DIR})

# Sqlite for reading the dump
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite ${CMAKE_CURRENT_BINARY_DIR}/third_party/sqlite)

# Zlib for decompressing replays
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib ${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib)
# Replace .res.res files with .res, windows thing
execute_process(
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib/CMakeFiles/zlib.dir/win32/zlib1.res.res
		${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib/CMakeFiles/zlib.dir/win32/zlib1.res)
execute_process(
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib/zlib.h
		${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib/zlib.h)
execute_process(
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib/zlib.dll
		${CMAKE_CURRENT_BINARY_DIR}/zlib.dll)

# Libcurl for downloading Miis for visualization
set(CURL_LIB ${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/lib/libcurl.lib)
set(CURL_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/include)
execute_process(
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/lib/libcurl.dll
		${CMAKE_CURRENT_BINARY_DIR}/libcurl.dll)

# SDL for OpenGL canvas creation
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdl ${CMAKE_CURRENT_BINARY_DIR}/third_party/sdl)
execute_process(
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_BINARY_DIR}/third_party/sdl/CMakeFiles/SDL2.dir/src/main/windows/version.rc.res.res
		${CMAKE_CURRENT_BINARY_DIR}/third_party/sdl/CMakeFiles/SDL2.dir/src/main/windows/version.rc.res)
execute_process(
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_BINARY_DIR}/third_party/sdl/SDL2d.dll
		${CMAKE_CURRENT_BINARY_DIR}/SDL2d.dll)

# FFmpeg for rendering the final video
set(AVCODEC_LIB ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/lib/avcodec.lib)
set(AVUTIL_LIB ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/lib/avutil.lib)
set(AVFORMAT_LIB ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/lib/avformat.lib)
execute_process(
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/avcodec-59.dll
		${CMAKE_CURRENT_BINARY_DIR}/avcodec-59.dll)
execute_process(
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/avutil-57.dll
		${CMAKE_CURRENT_BINARY_DIR}/avutil-57.dll)
execute_process(
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/swresample-4.dll
		${CMAKE_CURRENT_BINARY_DIR}/swresample-4.dll)
execute_process(
	COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/bin/avformat-59.dll
		${CMAKE_CURRENT_BINARY_DIR}/avformat-59.dll)
set(FFMPEG_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/include)

add_executable(ninjireplay ${APPLICATION_TYPE}
	src/glad.c
	src/main.cpp
)

target_include_directories(ninjireplay PUBLIC include src ${SKIA_DIR} ${SKIA_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite ${CURL_INCLUDE} ${FFMPEG_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdl/include)
target_compile_options(ninjireplay PRIVATE -Wall -Wextra)

set_target_properties(ninjireplay PROPERTIES
		OUTPUT_NAME "ninjireplay"
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		CXX_VISIBILITY_PRESET hidden
		POSITION_INDEPENDENT_CODE ON)

target_link_libraries(ninjireplay PUBLIC ${SKIA_LINK}/skia.lib sqlite zlib ${CURL_LIB} SDL2 SDL2main ${AVCODEC_LIB} ${AVUTIL_LIB} ${AVFORMAT_LIB} gdi32 opengl32)