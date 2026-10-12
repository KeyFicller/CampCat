# The gate's C++ half. One executable per unit under test; link it against
# campcat_core so tests use the same compile flags and OpenCV as the app.
add_executable(test_campcat_core cpp/test_approval_gate.cpp
                                 cpp/test_automation_log.cpp
                                 cpp/test_ccat_cond_action.cpp
                                 cpp/test_ccat_condition_walks.cpp
                                 cpp/test_ccat_return.cpp
                                 cpp/test_llm_protocol.cpp)
target_link_libraries(test_campcat_core PRIVATE campcat_core GTest::gtest_main)

# Each TEST becomes its own ctest entry, so a failure names the case that broke
# rather than just the binary. This reads the test list by running the binary,
# so ctest has to come after a build -- `scripts/test.sh` keeps that order.
include(GoogleTest)
gtest_discover_tests(test_campcat_core)
