add_executable(CalcTest
  CalculatorTest.cpp
  ${REPO_ROOT}/lib/Calc/Calculator.cpp
)

target_link_libraries(CalcTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(CalcTest PRIVATE ${REPO_ROOT}/lib/Calc)

gtest_discover_tests(CalcTest)
