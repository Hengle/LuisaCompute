add_library(luisa-compute-ext INTERFACE)

#set(WITH_WERROR OFF CACHE BOOL "" FORCE)
#set(SKIP_PERFORMANCE_COMPARISON ON CACHE BOOL "" FORCE)
#add_subdirectory(cereal)
#target_link_libraries(luisa-compute-ext INTERFACE cereal::cereal)
target_include_directories(luisa-compute-ext INTERFACE cereal/include)

set(SPDLOG_BUILD_SHARED ON CACHE BOOL "" FORCE)
set(SPDLOG_ENABLE_PCH ON CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL_HO ON CACHE BOOL "" FORCE)
add_subdirectory(fmt)
add_subdirectory(spdlog)
target_link_libraries(luisa-compute-ext INTERFACE spdlog::spdlog)

find_package(Threads REQUIRED)
target_link_libraries(luisa-compute-ext INTERFACE Threads::Threads)

add_subdirectory(xxHash/cmake_unofficial)
target_link_libraries(luisa-compute-ext INTERFACE xxHash::xxhash)
