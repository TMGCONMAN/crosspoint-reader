add_executable(TextWrapTest
  TextWrapTest.cpp
  ${REPO_ROOT}/lib/TextWrap/TextWrap.cpp
)

target_link_libraries(TextWrapTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(TextWrapTest PRIVATE ${REPO_ROOT}/lib/TextWrap)

gtest_discover_tests(TextWrapTest)
