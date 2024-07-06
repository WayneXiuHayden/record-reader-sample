cmake_minimum_required(VERSION 3.10)
project(MinimalGStreamerExample)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.16 gstreamer-video-1.0>=1.16 gstreamer-app-1.0>=1.16)

# Set the default build type if none was specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Define build type options
set(CMAKE_CXX_FLAGS_DEBUG "-g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_STANDARD 17)

add_executable(minimal_example main.cpp)

target_include_directories(minimal_example PRIVATE ${GST_INCLUDE_DIRS})
target_link_libraries(minimal_example PRIVATE ${GST_LIBRARIES})
