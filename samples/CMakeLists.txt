function(gen_bin name)

    add_executable(${name} ${name}.cpp)

    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)

    target_link_libraries(${name} PRIVATE interface_my3d)

endfunction()

gen_bin(main)
