add_subdirectory(consensus)

add_executable(test-tvm-hotpath-stats test-tvm-hotpath-stats.cpp)
target_link_libraries(test-tvm-hotpath-stats PRIVATE
    td-test-main
    tdutils
    ton_crypto
)
target_include_directories(test-tvm-hotpath-stats PRIVATE ${CMAKE_SOURCE_DIR}/validator)
ton_test(test-tvm-hotpath-stats)

add_executable(test-parallel-inbound-scheduler
    test-parallel-inbound-scheduler.cpp
    ${CMAKE_SOURCE_DIR}/validator/impl/parallel-inbound-scheduler.cpp
)
target_link_libraries(test-parallel-inbound-scheduler PRIVATE
    td-test-main
    tdutils
)
target_include_directories(test-parallel-inbound-scheduler PRIVATE ${CMAKE_SOURCE_DIR}/validator)
ton_test(test-parallel-inbound-scheduler)

add_executable(test-parallel-cell-usage-journal
    test-parallel-cell-usage-journal.cpp
    ${CMAKE_SOURCE_DIR}/validator/impl/parallel-cell-usage-journal.cpp
)
target_link_libraries(test-parallel-cell-usage-journal PRIVATE
    td-test-main
    tdutils
    ton_crypto
)
target_include_directories(test-parallel-cell-usage-journal PRIVATE ${CMAKE_SOURCE_DIR}/validator)
ton_test(test-parallel-cell-usage-journal)

add_executable(test-parallel-transaction-payload
    test-parallel-transaction-payload.cpp
    ${CMAKE_SOURCE_DIR}/validator/impl/parallel-coordinator-shadow.cpp
    ${CMAKE_SOURCE_DIR}/validator/impl/parallel-inbound-scheduler.cpp
    ${CMAKE_SOURCE_DIR}/validator/impl/parallel-cell-usage-journal.cpp
    ${CMAKE_SOURCE_DIR}/validator/impl/parallel-transaction-payload.cpp
)
target_link_libraries(test-parallel-transaction-payload PRIVATE
    td-test-main
    tdutils
    ton_crypto
)
target_include_directories(test-parallel-transaction-payload PRIVATE ${CMAKE_SOURCE_DIR}/validator)
ton_test(test-parallel-transaction-payload)
