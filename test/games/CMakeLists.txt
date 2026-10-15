add_executable(GamesTest
  Game2048Test.cpp
  SudokuTest.cpp
  ChessTest.cpp
  CheckersTest.cpp
  ${REPO_ROOT}/lib/Games/Game2048.cpp
  ${REPO_ROOT}/lib/Games/Sudoku.cpp
  ${REPO_ROOT}/lib/Games/Chess.cpp
  ${REPO_ROOT}/lib/Games/Checkers.cpp
)

target_link_libraries(GamesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(GamesTest PRIVATE ${REPO_ROOT}/lib/Games)

gtest_discover_tests(GamesTest)
