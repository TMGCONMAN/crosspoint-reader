add_executable(MeshCompanionTest
  MeshFramesTest.cpp
  MeshCompanionClientTest.cpp
  MeshThreadLogTest.cpp
  MeshContactBookTest.cpp
  MeshSendTrackerTest.cpp
  ${REPO_ROOT}/lib/MeshCompanion/MeshFrames.cpp
  ${REPO_ROOT}/lib/MeshCompanion/MeshCompanionClient.cpp
  ${REPO_ROOT}/lib/MeshCompanion/MeshThreadLog.cpp
  ${REPO_ROOT}/lib/MeshCompanion/MeshContactBook.cpp
  ${REPO_ROOT}/lib/MeshCompanion/MeshSendTracker.cpp
)

target_link_libraries(MeshCompanionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(MeshCompanionTest PRIVATE ${REPO_ROOT}/lib/MeshCompanion)

gtest_discover_tests(MeshCompanionTest)
