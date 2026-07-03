function(wss_add_example name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE wss::wss)
    wss_set_warnings(${name})
endfunction()

# Examples are added here as they're implemented in later phases.
