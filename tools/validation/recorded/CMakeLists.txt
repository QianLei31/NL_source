cmake_minimum_required(VERSION 3.20)
project(NLRecordedSampleValidation LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Concurrent)
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(sample_qa sample_qa.cpp
    ${REPO}/src/network/replay_controller.cpp ${REPO}/src/network/replay_controller.h
    ${REPO}/src/network/timestamp_checker.cpp ${REPO}/src/network/timestamp_checker.h
    ${REPO}/src/io/session_manifest.cpp)
add_executable(sample_unverified_qa sample_unverified_qa.cpp
    ${REPO}/src/network/replay_controller.cpp ${REPO}/src/network/replay_controller.h
    ${REPO}/src/io/session_manifest.cpp
    ${REPO}/src/service/spike_detect_worker.cpp ${REPO}/src/service/spike_detect_worker.h
    ${REPO}/src/signal/spike_snippet_store.cpp ${REPO}/src/signal/spike_filter.cpp)
foreach(target sample_qa sample_unverified_qa)
    target_include_directories(${target} PRIVATE ${REPO}/src)
    target_link_libraries(${target} PRIVATE Qt6::Core Qt6::Concurrent)
endforeach()
