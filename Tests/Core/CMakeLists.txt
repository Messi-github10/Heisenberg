include(GoogleTest)

add_executable(HeisenbergCoreTests
    profile_test.cpp
)
target_link_libraries(HeisenbergCoreTests PRIVATE
    HeisenbergCore
    GTest::gtest_main
)

if(MSVC)
    target_compile_options(HeisenbergCoreTests PRIVATE /utf-8)
endif()

gtest_discover_tests(HeisenbergCoreTests)
