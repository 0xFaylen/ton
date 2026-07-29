add_subdirectory(consensus)

add_executable(test-tvm-hotpath-stats test-tvm-hotpath-stats.cpp)
target_link_libraries(test-tvm-hotpath-stats PRIVATE
    td-test-main
    tdutils
    ton_crypto
)
target_include_directories(test-tvm-hotpath-stats PRIVATE ${CMAKE_SOURCE_DIR}/validator)
ton_test(test-tvm-hotpath-stats)
