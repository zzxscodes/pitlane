cmake_minimum_required(VERSION 3.16)
project(pitlane VERSION 2.2.1 LANGUAGES CXX)

set(PITLANE_SOURCES
    src/desk/broker.cpp
    src/desk/cash.cpp
    src/desk/codec.cpp
    src/desk/feed.cpp
    src/desk/logline.cpp
    src/desk/names.cpp
    src/desk/region.cpp
    src/desk/runtime.cpp
    src/desk/signal.cpp
    src/desk/store.cpp
    src/book/calendar.cpp
    src/book/cross.cpp
    src/book/venue.cpp
    src/order/lots.cpp
    src/time/simclock.cpp
    src/util/textutil.cpp)

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

set(PITLANE_EXAMPLES demo desk book order study time lane own pace)
foreach(ex ${PITLANE_EXAMPLES})
    if(ex STREQUAL "study")
        set(ex_tgt pitlane_ex_study)
    else()
        set(ex_tgt pitlane_${ex})
    endif()
    add_executable(${ex_tgt} examples/${ex}.cpp)
    target_link_libraries(${ex_tgt} PRIVATE pitlane)
    target_compile_options(${ex_tgt} PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)
endforeach()

add_executable(pitlane_assist tests/assist.cpp)
target_link_libraries(pitlane_assist PRIVATE pitlane)
target_compile_options(pitlane_assist PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_next tests/next.cpp)
target_link_libraries(pitlane_next PRIVATE pitlane)
target_compile_options(pitlane_next PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_infra tests/infra.cpp)
target_link_libraries(pitlane_infra PRIVATE pitlane)
target_compile_options(pitlane_infra PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_drill tests/drill.cpp)
target_link_libraries(pitlane_drill PRIVATE pitlane)
target_compile_options(pitlane_drill PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_lat tests/lat.cpp)
target_link_libraries(pitlane_lat PRIVATE pitlane)
target_compile_options(pitlane_lat PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_perf tests/perf.cpp)
target_link_libraries(pitlane_perf PRIVATE pitlane)
target_compile_options(pitlane_perf PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_sheet tests/sheet.cpp)
target_link_libraries(pitlane_sheet PRIVATE pitlane)
target_compile_options(pitlane_sheet PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_study tests/study.cpp)
target_link_libraries(pitlane_study PRIVATE pitlane)
target_compile_options(pitlane_study PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

add_executable(pitlane_extend tests/extend.cpp)
target_link_libraries(pitlane_extend PRIVATE pitlane)
target_compile_options(pitlane_extend PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

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

    add_executable(pitlane_infra_shared tests/infra.cpp)
    target_link_libraries(pitlane_infra_shared PRIVATE pitlane_shared)
    target_compile_options(pitlane_infra_shared PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

    add_executable(pitlane_drill_shared tests/drill.cpp)
    target_link_libraries(pitlane_drill_shared PRIVATE pitlane_shared)
    target_compile_options(pitlane_drill_shared PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

    add_executable(pitlane_lat_shared tests/lat.cpp)
    target_link_libraries(pitlane_lat_shared PRIVATE pitlane_shared)
    target_compile_options(pitlane_lat_shared PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

    add_executable(pitlane_perf_shared tests/perf.cpp)
    target_link_libraries(pitlane_perf_shared PRIVATE pitlane_shared)
    target_compile_options(pitlane_perf_shared PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

    add_executable(pitlane_sheet_shared tests/sheet.cpp)
    target_link_libraries(pitlane_sheet_shared PRIVATE pitlane_shared)
    target_compile_options(pitlane_sheet_shared PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

    add_executable(pitlane_study_shared tests/study.cpp)
    target_link_libraries(pitlane_study_shared PRIVATE pitlane_shared)
    target_compile_options(pitlane_study_shared PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)

    add_executable(pitlane_extend_shared tests/extend.cpp)
    target_link_libraries(pitlane_extend_shared PRIVATE pitlane_shared)
    target_compile_options(pitlane_extend_shared PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)
endif()

option(PITLANE_SANITIZE "Build with address and undefined-behavior sanitizers" OFF)
if(PITLANE_SANITIZE)
    set(PITLANE_SAN_TARGETS pitlane pitlane_check pitlane_suite pitlane_demo pitlane_desk pitlane_book pitlane_order pitlane_ex_study pitlane_time pitlane_lane pitlane_own pitlane_pace pitlane_assist pitlane_next pitlane_infra pitlane_drill pitlane_lat pitlane_perf pitlane_sheet pitlane_study pitlane_extend)
    if(PITLANE_BUILD_SHARED)
        list(APPEND PITLANE_SAN_TARGETS pitlane_shared pitlane_check_shared pitlane_assist_shared pitlane_next_shared pitlane_infra_shared pitlane_drill_shared pitlane_lat_shared pitlane_perf_shared pitlane_sheet_shared pitlane_study_shared pitlane_extend_shared)
    endif()
    foreach(tgt ${PITLANE_SAN_TARGETS})
        target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -O1)
        target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
    endforeach()
    target_compile_definitions(pitlane_perf PRIVATE PITLANE_RELAX_LATENCY=1)
    if(PITLANE_BUILD_SHARED)
        target_compile_definitions(pitlane_perf_shared PRIVATE PITLANE_RELAX_LATENCY=1)
    endif()
endif()

enable_testing()
add_test(NAME check COMMAND pitlane_check)
add_test(NAME suite COMMAND pitlane_suite)
add_test(NAME assist COMMAND pitlane_assist)
add_test(NAME next COMMAND pitlane_next)
add_test(NAME infra COMMAND pitlane_infra)
add_test(NAME drill COMMAND pitlane_drill)
add_test(NAME lat COMMAND pitlane_lat)
add_test(NAME perf COMMAND pitlane_perf)
set_tests_properties(perf PROPERTIES RUN_SERIAL TRUE)
add_test(NAME sheet COMMAND pitlane_sheet)
add_test(NAME study COMMAND pitlane_study)
add_test(NAME extend COMMAND pitlane_extend)
add_test(NAME demo COMMAND pitlane_demo)
add_test(NAME ex_desk COMMAND pitlane_desk)
add_test(NAME ex_book COMMAND pitlane_book)
add_test(NAME ex_order COMMAND pitlane_order)
add_test(NAME ex_study COMMAND pitlane_ex_study)
add_test(NAME ex_time COMMAND pitlane_time)
add_test(NAME ex_lane COMMAND pitlane_lane)
add_test(NAME ex_own COMMAND pitlane_own)
add_test(NAME ex_pace COMMAND pitlane_pace)
if(PITLANE_BUILD_SHARED)
    add_test(NAME check_shared COMMAND pitlane_check_shared)
    add_test(NAME assist_shared COMMAND pitlane_assist_shared)
    add_test(NAME next_shared COMMAND pitlane_next_shared)
    add_test(NAME infra_shared COMMAND pitlane_infra_shared)
    add_test(NAME drill_shared COMMAND pitlane_drill_shared)
    add_test(NAME lat_shared COMMAND pitlane_lat_shared)
    add_test(NAME perf_shared COMMAND pitlane_perf_shared)
    set_tests_properties(perf_shared PROPERTIES RUN_SERIAL TRUE)
    add_test(NAME sheet_shared COMMAND pitlane_sheet_shared)
    add_test(NAME study_shared COMMAND pitlane_study_shared)
    add_test(NAME extend_shared COMMAND pitlane_extend_shared)
endif()

install(TARGETS ${PITLANE_INSTALL_TARGETS}
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib)
install(DIRECTORY include/pitlane DESTINATION include)
