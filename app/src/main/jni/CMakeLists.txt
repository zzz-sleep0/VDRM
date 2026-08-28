cmake_minimum_required(VERSION 3.10)
project(vdrm_consumer)

add_library(vdrm_consumer SHARED native_consumer.c)
target_include_directories(vdrm_consumer PRIVATE .)
target_link_libraries(vdrm_consumer log android aaudio EGL GLESv2 dl)