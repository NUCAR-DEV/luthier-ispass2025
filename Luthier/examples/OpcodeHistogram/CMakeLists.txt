cmake_minimum_required(VERSION 3.21)
project(LuthierOpcodeHistogram LANGUAGES HIP CXX)

set(CMAKE_HIP_STANDARD 20)

find_package(hip REQUIRED)

find_package(LLVM REQUIRED CONFIG)

add_library(LuthierOpcodeHistogram SHARED OpcodeHistogram.hip)

luthier_add_compiler_plugin(LuthierOpcodeHistogram luthier::IModuleEmbedPlugin)

target_link_libraries(LuthierOpcodeHistogram PUBLIC LuthierTooling LLVMDemangle hip::device hip::host)