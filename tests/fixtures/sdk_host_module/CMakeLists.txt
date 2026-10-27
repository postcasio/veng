# A standalone consumer of veng_add_host_shader_module, built by the SDK conformance tests from an
# installed and a build-tree SDK: it compiles a Slang module for the host and runs it.
cmake_minimum_required(VERSION 4.1)
project(veng_sdk_host_module LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)

find_package(veng REQUIRED)

veng_add_host_shader_module(scale_host
        MODULE scale_host.slang
        NAME ScaleHost)

add_executable(host_module_check main.cpp)
target_link_libraries(host_module_check PRIVATE scale_host)
