cmake_minimum_required(VERSION 3.16)
project(pitlane VERSION 1.2.0 LANGUAGES CXX)

set(PITLANE_SOURCES
    src/region.cpp
    src/cash.cpp
    src/cross.cpp
    src/venue.cpp
    src/codec.cpp
    src/calendar.cpp
    src/lots.cpp
    src/feed.cpp
    src/textutil.cpp
    src/simclock.cpp
    src/store.cpp
    src/names.cpp
    src/signal.cpp
    src/runtime.cpp
    src/broker.cpp
    src/logline.cpp)

# Objects are position-independent, so libpitlane.a can also be linked into another shared library.
option(PITLANE_BUILD_SHARED "Build libpitlane.so in addition to the static library" ON)

function(pitlane_apply target)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

add_library(pitlane STATIC ${PITLANE_SOURCES})
pitlane_apply(pitlane)

set(PITLANE_INSTALL_TARGETS pitlane)
if(PITLANE_BUILD_SHARED)
    add_library(pitlane_shared SHARED ${PITLANE_SOURCES})
    pitlane_apply(pitlane_shared)
    set_target_properties(pitlane_shared PROPERTIES
        OUTPUT_NAME pitlane
        VERSION ${PROJECT_VERSION}
        SOVERSION 1)
    list(APPEND PITLANE_INSTALL_TARGETS pitlane_shared)
endif()

add_executable(pitlane_check tests/check.cpp)
target_link_libraries(pitlane_check PRIVATE pitlane)
target_compile_options(pitlane_check PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_suite tests/suite.cpp)
target_link_libraries(pitlane_suite PRIVATE pitlane)
target_compile_options(pitlane_suite PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_demo examples/demo.cpp)
target_link_libraries(pitlane_demo PRIVATE pitlane)
target_compile_options(pitlane_demo PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_assist tests/assist.cpp)
target_link_libraries(pitlane_assist PRIVATE pitlane)
target_compile_options(pitlane_assist PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_next tests/next.cpp)
target_link_libraries(pitlane_next PRIVATE pitlane)
target_compile_options(pitlane_next PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

if(PITLANE_BUILD_SHARED)
    add_executable(pitlane_check_shared tests/check.cpp)
    target_link_libraries(pitlane_check_shared PRIVATE pitlane_shared)
    target_compile_options(pitlane_check_shared PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

    add_executable(pitlane_assist_shared tests/assist.cpp)
    target_link_libraries(pitlane_assist_shared PRIVATE pitlane_shared)
    target_compile_options(pitlane_assist_shared PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

    add_executable(pitlane_next_shared tests/next.cpp)
    target_link_libraries(pitlane_next_shared PRIVATE pitlane_shared)
    target_compile_options(pitlane_next_shared PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)
endif()

option(PITLANE_SANITIZE "Build with address and undefined-behavior sanitizers" OFF)
if(PITLANE_SANITIZE)
    set(PITLANE_SAN_TARGETS pitlane pitlane_check pitlane_suite pitlane_demo pitlane_assist pitlane_next)
    if(PITLANE_BUILD_SHARED)
        list(APPEND PITLANE_SAN_TARGETS pitlane_shared pitlane_check_shared pitlane_assist_shared pitlane_next_shared)
    endif()
    foreach(tgt ${PITLANE_SAN_TARGETS})
        target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -O1)
        target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

enable_testing()
add_test(NAME check COMMAND pitlane_check)
add_test(NAME suite COMMAND pitlane_suite)
add_test(NAME assist COMMAND pitlane_assist)
add_test(NAME next COMMAND pitlane_next)
if(PITLANE_BUILD_SHARED)
    add_test(NAME check_shared COMMAND pitlane_check_shared)
    add_test(NAME assist_shared COMMAND pitlane_assist_shared)
    add_test(NAME next_shared COMMAND pitlane_next_shared)
endif()

install(TARGETS ${PITLANE_INSTALL_TARGETS}
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib)
install(DIRECTORY include/pitlane DESTINATION include)
