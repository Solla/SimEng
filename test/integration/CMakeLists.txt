set(TEST_SOURCES
<<<<<<< HEAD
    ConfigTest.cc
)
=======
    VirtMemTests.cc
    ../testenv/FilesEnv.cc
    )
>>>>>>> origin/dynamic-linking-support

add_executable(integrationtests ${TEST_SOURCES})

target_include_directories(integrationtests PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(integrationtests PUBLIC ${PROJECT_SOURCE_DIR}/src/lib)
target_link_libraries(integrationtests libsimeng)
target_link_libraries(integrationtests gmock_main)
<<<<<<< HEAD
target_compile_options(integrationtests PRIVATE ${SIMENG_COMPILE_OPTIONS})
=======
>>>>>>> origin/dynamic-linking-support

add_test(NAME integration_tests COMMAND integrationtests)
