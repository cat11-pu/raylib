cmake_minimum_required(VERSION 3.22)

if (NOT TARGET raylib)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." raylib)
endif ()

add_executable(resource_hot_reload resource_hot_reload.c)
target_link_libraries(resource_hot_reload PRIVATE raylib)
target_compile_features(resource_hot_reload PRIVATE c_std_11)
target_compile_definitions(resource_hot_reload PRIVATE
    ASSET_ROOT="${CMAKE_CURRENT_LIST_DIR}/../.."
)

if (NOT WIN32)
    target_link_libraries(resource_hot_reload PRIVATE m)
endif ()

add_test(NAME resource_hot_reload COMMAND resource_hot_reload)
set_tests_properties(resource_hot_reload PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1"
)
