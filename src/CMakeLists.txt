cmake_minimum_required(VERSION 3.6)

project("gtest")

set(ROOT_SRC "${CMAKE_SOURCE_DIR}")
# 指定源文件所在的目录
set(SOURCE_DIR "${CMAKE_SOURCE_DIR}/")

# add_definitions(-DGST_DEBUG)
add_definitions(-DGST_LEVEL=5) 

# 搜索目录中的所有 .c 文件
file(GLOB_RECURSE SOURCES "${SOURCE_DIR}/*.c")

message("Found source files: ${SOURCES}")

add_executable(gtest ${SOURCES})

# 查找 pkg-config 工具
find_package(PkgConfig REQUIRED)

# 使用 pkg-config 查找 GStreamer 和 GLib 的编译和链接标志
pkg_check_modules(GLIB REQUIRED glib-2.0 gobject-2.0)
# pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
# 检测 FFmpeg 依赖
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libavfilter libswscale)
# 检查 SDL2 依赖
pkg_check_modules(SDL REQUIRED sdl2)

message("GLib include directories: ${GLIB_INCLUDE_DIRS}")
message("GLib libraries: ${GLIB_LIBRARIES}")
# message("GStreamer libraries: ${GSTREAMER_LIBRARIES}")
message("SDL include directories: ${SDL_INCLUDE_DIRS}")
message("SDL libraries: ${SDL_LIBRARIES}")

# 设置包含目录
target_include_directories(gtest PUBLIC 
                                ${ROOT_SRC}/audioplayer
                                ${ROOT_SRC}/elements
                                ${ROOT_SRC}/videoplayer
                                ${ROOT_SRC}/mediaplayer
                                ${ROOT_SRC}/../include/gstreamer-1.0/ges 
                                ${GLIB_INCLUDE_DIRS}
                                ${FFMPEG_INCLUDE_DIRS}
                                ${SDL_INCLUDE_DIRS}
                                ${ROOT_SRC}/../include/gstreamer-1.0/)

# 设置链接目录
target_link_directories(gtest PUBLIC 
                                ${ROOT_SRC}/../lib/gstreamer-1.0
                                ${ROOT_SRC}/../lib/gstreamer-1.0/validate
                                ${ROOT_SRC}/../lib/gst-validate-launcher
                                ${ROOT_SRC}/../lib/graphene-1.0
                                ${ROOT_SRC}/../lib/girepository-1.0
                                ${ROOT_SRC}/../lib
                                ${GLIB_LIBRARY_DIRS}
                                ${FFMPEG_LIBRARY_DIRS}
                                ${SDL_LIBRARY_DIRS})

# 递归搜索所有子目录中的 .so 文件
file(GLOB_RECURSE LIBS "${ROOT_SRC}/../lib/**.dylib")

message("Found shared libraries: ${LIBS}")

# 链接库
target_link_libraries(gtest PRIVATE ${LIBS} ${GLIB_LIBRARIES} ${GSTREAMER_LIBRARIES} ${FFMPEG_LIBRARIES} ${SDL_LIBRARIES})