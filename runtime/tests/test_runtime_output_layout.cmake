cmake_minimum_required(VERSION 3.19)
# Compatibility entry for the real-product layout regression. Final identity is
# available after linking, so a script-mode variable model cannot test it.
if(NOT TEST_RUNTIME_SOURCE_DIR OR NOT TEST_RUNTIME_OUTPUT_WORK)
    message(FATAL_ERROR "Set TEST_RUNTIME_SOURCE_DIR and TEST_RUNTIME_OUTPUT_WORK (outside the source tree)")
endif()
execute_process(COMMAND python3 "${TEST_RUNTIME_SOURCE_DIR}/tests/test_runtime_output_product.py"
    --source "${TEST_RUNTIME_SOURCE_DIR}" --work "${TEST_RUNTIME_OUTPUT_WORK}"
    --mode override --build RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "real-product output layout regression failed: ${_rc}")
endif()
message(STATUS "RUNTIME_OUTPUT_LAYOUT_OK real-product override isolation/restoration")
