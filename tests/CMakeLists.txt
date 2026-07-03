include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

function(wss_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE wss::wss GTest::gtest_main)
    wss_set_warnings(${name})
    wss_apply_sanitizers(${name})
    gtest_discover_tests(${name})
endfunction()

wss_add_test(stub_test)
wss_add_test(primitives_test)
wss_add_test(chase_lev_deque_test)
wss_add_test(idle_signal_test)
wss_add_test(injector_test)
wss_add_test(basic_execution_test)
wss_add_test(shutdown_test)
wss_add_test(cancellation_test)
wss_add_test(work_stealing_test)
wss_add_test(stress_test)
wss_add_test(global_queue_scheduler_test)
