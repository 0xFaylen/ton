# Runs tvm-replay-bundle against the checked-in mainnet block 88028077 fixture.
# The witness-mode replay is fail-closed: it exits non-zero unless the complete
# account-proof scope replays exactly and all four augmented dictionary roots
# (ShardAccounts, InMsgDescr, OutMsgDescr, OutMsgQueue) validate. The output
# markers below pin the expected fixture coverage so a silently narrowed replay
# cannot pass.
if(NOT REPLAY_TOOL OR NOT FIXTURE_DIR)
  message(FATAL_ERROR "REPLAY_TOOL and FIXTURE_DIR are required")
endif()

file(GLOB account_proofs "${FIXTURE_DIR}/account-proof-*.tl")
list(LENGTH account_proofs proof_count)
if(NOT proof_count EQUAL 108)
  message(FATAL_ERROR "expected 108 account proofs in ${FIXTURE_DIR}, found ${proof_count}")
endif()

set(replay_command
    "${REPLAY_TOOL}"
    --block-boc "${FIXTURE_DIR}/block-88028077.boc"
    --block-id
    "(0,8000000000000000,88028077):3D60B72C796B49E117A9E2FD3AA451E1D4D51D719E5713F8975D7593C7E557A4:EDC7BAA4189751C07CBF423D3EC1DCB6502C192C52428D2E20F6F3D3837D2C6F"
    --mc-proof "${FIXTURE_DIR}/config-proof.tl"
    --block-update-witness
    --out-msg-queue-proof "${FIXTURE_DIR}/outmsgqueueproof-88028076.tl"
    --library-bodies "${FIXTURE_DIR}/libraries-body.tl"
    --library-bodies "${FIXTURE_DIR}/libraries-transitive.tl"
    --account-workers 2
    --account-samples 1
    --offline-collator-workers 2)
foreach(proof IN LISTS account_proofs)
  get_filename_component(proof_name "${proof}" NAME_WE)
  string(REPLACE "account-proof-" "" account "${proof_name}")
  list(APPEND replay_command --account-proof "${account}=${proof}")
endforeach()

execute_process(
  COMMAND ${replay_command}
  OUTPUT_VARIABLE replay_output
  ERROR_VARIABLE replay_error
  RESULT_VARIABLE replay_result)
if(NOT replay_result EQUAL 0)
  message(FATAL_ERROR "four-root fixture replay failed (${replay_result}): ${replay_error}")
endif()

string(REGEX MATCHALL "\"augmented_dictionary_roots_validated\":[0-9]+" root_reports "${replay_output}")
list(LENGTH root_reports root_report_count)
if(NOT root_report_count EQUAL 2)
  message(FATAL_ERROR "expected 2 augmented-dictionary root reports (serial replay and offline collator probe), "
                      "found ${root_report_count}")
endif()
foreach(report IN LISTS root_reports)
  if(NOT report STREQUAL "\"augmented_dictionary_roots_validated\":4")
    message(FATAL_ERROR "an augmented-root gate did not validate four roots: ${report}")
  endif()
endforeach()

string(REGEX MATCHALL "\"augmented_roots_validated\":[0-9]+" probe_reports "${replay_output}")
list(LENGTH probe_reports probe_report_count)
if(NOT probe_report_count EQUAL 1)
  message(FATAL_ERROR "expected 1 parallel account-probe root report, found ${probe_report_count}")
endif()
list(GET probe_reports 0 probe_report)
if(NOT probe_report STREQUAL "\"augmented_roots_validated\":4")
  message(FATAL_ERROR "the parallel account probe did not validate four roots: ${probe_report}")
endif()

set(required_markers
    "\"scope\":\"full_block\""
    "\"predecessor_state_witness_source\":\"target_block_state_update_hindsight\""
    "\"in_msg_descriptors_bound\":138"
    "\"out_msg_descriptors_bound\":114"
    "\"complete_message_descriptor_coverage\":true"
    "\"target_accounts\":108")
foreach(marker IN LISTS required_markers)
  string(FIND "${replay_output}" "${marker}" marker_offset)
  if(marker_offset EQUAL -1)
    message(FATAL_ERROR "required replay output marker is missing: ${marker}")
  endif()
endforeach()

message(STATUS "four-root fixture gate passed: 108 accounts, 138/138 InMsg, 114/114 OutMsg, 4/4 roots")
