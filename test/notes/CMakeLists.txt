add_executable(NotesTest
  NoteDocumentTest.cpp
  ${REPO_ROOT}/lib/Notes/NoteDocument.cpp
)

target_link_libraries(NotesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(NotesTest PRIVATE ${REPO_ROOT}/lib/Notes)

gtest_discover_tests(NotesTest)
