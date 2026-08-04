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

add_executable(test-parallel-worker-pool
    test-parallel-worker-pool.cpp
    ${CMAKE_SOURCE_DIR}/validator/impl/parallel-worker-pool.cpp
)
target_link_libraries(test-parallel-worker-pool PRIVATE
    td-test-main
    tdutils
)
target_include_directories(test-parallel-worker-pool PRIVATE ${CMAKE_SOURCE_DIR}/validator)
ton_test(test-parallel-worker-pool)

add_executable(test-parallel-merkle-proof-merge
    test-parallel-merkle-proof-merge.cpp
    ${CMAKE_SOURCE_DIR}/validator/impl/parallel-merkle-proof-merge.cpp
)
target_link_libraries(test-parallel-merkle-proof-merge PRIVATE
    td-test-main
    tdutils
    ton_crypto
)
target_include_directories(test-parallel-merkle-proof-merge PRIVATE ${CMAKE_SOURCE_DIR}/validator)
ton_test(test-parallel-merkle-proof-merge)

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

add_executable(test-selective-split-state
    test-selective-split-state.cpp
    ${CMAKE_SOURCE_DIR}/validator/impl/selective-split-state.cpp
)
target_link_libraries(test-selective-split-state PRIVATE
    td-test-main
    tdutils
    ton_crypto
    ton_block
)
target_include_directories(test-selective-split-state PRIVATE ${CMAKE_SOURCE_DIR}/validator)
ton_test(test-selective-split-state)

add_executable(test-block-workload-fixture
    test-block-workload-fixture.cpp
    ${CMAKE_SOURCE_DIR}/validator/validation-replay/block-workload.cpp
)
target_link_libraries(test-block-workload-fixture PRIVATE
    td-test-main
    tdutils
    ton_crypto
    ton_block
)
target_include_directories(test-block-workload-fixture PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/validator)
target_compile_definitions(test-block-workload-fixture PRIVATE
    TON_REPLAY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
ton_test(test-block-workload-fixture)

if(TARGET tvm-replay-bundle)
  add_test(NAME tvm-replay-four-root-fixture
    COMMAND ${CMAKE_COMMAND}
      -DREPLAY_TOOL=$<TARGET_FILE:tvm-replay-bundle>
      "-DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/four-root-88028077"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run-four-root-fixture.cmake")
  add_dependencies(all-tests tvm-replay-bundle)
endif()
