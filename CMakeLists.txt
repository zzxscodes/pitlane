cmake_minimum_required(VERSION 3.16)
project(pitlane VERSION 2.5.0 LANGUAGES CXX)

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
option(PITLANE_NATIVE "Emit code for the build-host CPU (-march=native)" ON)
option(PITLANE_LTO "Enable link-time optimization across translation units" ON)
option(PITLANE_SANITIZE "Build with address and undefined-behavior sanitizers" OFF)
set(PITLANE_PGO "OFF" CACHE STRING "Profile-guided optimization: OFF, GENERATE, or USE")
set_property(CACHE PITLANE_PGO PROPERTY STRINGS OFF GENERATE USE)

include(CheckIPOSupported)
check_ipo_supported(RESULT PITLANE_IPO_OK OUTPUT PITLANE_IPO_MSG)
find_package(Threads REQUIRED)

set(PITLANE_WARN_FLAGS -Wall -Wextra -Wpedantic -Werror)
set(PITLANE_OPT_FLAGS -O3 -fno-exceptions -fno-rtti -D_GNU_SOURCE)
if(PITLANE_NATIVE)
    list(APPEND PITLANE_OPT_FLAGS -march=native)
endif()
if(PITLANE_PGO STREQUAL "GENERATE")
    list(APPEND PITLANE_OPT_FLAGS -fprofile-generate)
    set(PITLANE_PGO_LINK -fprofile-generate)
elseif(PITLANE_PGO STREQUAL "USE")
    list(APPEND PITLANE_OPT_FLAGS -fprofile-use -fprofile-correction)
    set(PITLANE_PGO_LINK -fprofile-use)
else()
    set(PITLANE_PGO_LINK "")
endif()

function(pitlane_apply target)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_compile_options(${target} PRIVATE ${PITLANE_WARN_FLAGS} ${PITLANE_OPT_FLAGS})
    if(PITLANE_PGO_LINK)
        target_link_options(${target} PRIVATE ${PITLANE_PGO_LINK})
    endif()
    target_link_libraries(${target} PUBLIC Threads::Threads)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

function(pitlane_lto target)
    if(PITLANE_LTO AND PITLANE_IPO_OK AND NOT PITLANE_SANITIZE)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

add_library(pitlane STATIC ${PITLANE_SOURCES})
pitlane_apply(pitlane)
pitlane_lto(pitlane)

set(PITLANE_INSTALL_TARGETS pitlane)
if(PITLANE_BUILD_SHARED)
    add_library(pitlane_shared SHARED ${PITLANE_SOURCES})
    pitlane_apply(pitlane_shared)
    pitlane_lto(pitlane_shared)
    set_target_properties(pitlane_shared PROPERTIES
        OUTPUT_NAME pitlane
        VERSION ${PROJECT_VERSION}
        SOVERSION 1)
    list(APPEND PITLANE_INSTALL_TARGETS pitlane_shared)
endif()

set(PITLANE_EXAMPLES demo desk book order study time lane own pace)
foreach(ex ${PITLANE_EXAMPLES})
    if(ex STREQUAL "study")
        set(ex_tgt pitlane_ex_study)
    else()
        set(ex_tgt pitlane_${ex})
    endif()
    add_executable(${ex_tgt} examples/${ex}.cpp)
    target_link_libraries(${ex_tgt} PRIVATE pitlane)
    target_compile_options(${ex_tgt} PRIVATE ${PITLANE_WARN_FLAGS} ${PITLANE_OPT_FLAGS})
    if(PITLANE_PGO_LINK)
        target_link_options(${ex_tgt} PRIVATE ${PITLANE_PGO_LINK})
    endif()
    pitlane_lto(${ex_tgt})
endforeach()

set(PITLANE_AREAS desk book order time study util host)
foreach(area ${PITLANE_AREAS})
    add_executable(pitlane_test_${area} tests/${area}/check.cpp)
    target_link_libraries(pitlane_test_${area} PRIVATE pitlane)
    target_compile_options(pitlane_test_${area} PRIVATE ${PITLANE_WARN_FLAGS} ${PITLANE_OPT_FLAGS})
    if(PITLANE_PGO_LINK)
        target_link_options(pitlane_test_${area} PRIVATE ${PITLANE_PGO_LINK})
    endif()
    pitlane_lto(pitlane_test_${area})
endforeach()

add_executable(pitlane_perf tests/time/perf.cpp)
target_link_libraries(pitlane_perf PRIVATE pitlane)
target_compile_options(pitlane_perf PRIVATE ${PITLANE_WARN_FLAGS} ${PITLANE_OPT_FLAGS})
if(PITLANE_PGO_LINK)
    target_link_options(pitlane_perf PRIVATE ${PITLANE_PGO_LINK})
endif()
pitlane_lto(pitlane_perf)

if(PITLANE_BUILD_SHARED)
    foreach(area ${PITLANE_AREAS})
        add_executable(pitlane_test_${area}_shared tests/${area}/check.cpp)
        target_link_libraries(pitlane_test_${area}_shared PRIVATE pitlane_shared)
        target_compile_options(pitlane_test_${area}_shared PRIVATE ${PITLANE_WARN_FLAGS} ${PITLANE_OPT_FLAGS})
        if(PITLANE_PGO_LINK)
            target_link_options(pitlane_test_${area}_shared PRIVATE ${PITLANE_PGO_LINK})
        endif()
        pitlane_lto(pitlane_test_${area}_shared)
    endforeach()

    add_executable(pitlane_perf_shared tests/time/perf.cpp)
    target_link_libraries(pitlane_perf_shared PRIVATE pitlane_shared)
    target_compile_options(pitlane_perf_shared PRIVATE ${PITLANE_WARN_FLAGS} ${PITLANE_OPT_FLAGS})
    if(PITLANE_PGO_LINK)
        target_link_options(pitlane_perf_shared PRIVATE ${PITLANE_PGO_LINK})
    endif()
    pitlane_lto(pitlane_perf_shared)
endif()

if(PITLANE_SANITIZE)
    set(PITLANE_SAN_TARGETS pitlane pitlane_demo pitlane_desk pitlane_book pitlane_order pitlane_ex_study pitlane_time pitlane_lane pitlane_own pitlane_pace pitlane_test_desk pitlane_test_book pitlane_test_order pitlane_test_time pitlane_test_study pitlane_test_util pitlane_test_host pitlane_perf)
    if(PITLANE_BUILD_SHARED)
        list(APPEND PITLANE_SAN_TARGETS pitlane_shared pitlane_test_desk_shared pitlane_test_book_shared pitlane_test_order_shared pitlane_test_time_shared pitlane_test_study_shared pitlane_test_util_shared pitlane_test_host_shared pitlane_perf_shared)
    endif()
    foreach(tgt ${PITLANE_SAN_TARGETS})
        target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -O1)
        target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
        set_property(TARGET ${tgt} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    endforeach()
    target_compile_definitions(pitlane_perf PRIVATE PITLANE_RELAX_LATENCY=1)
    if(PITLANE_BUILD_SHARED)
        target_compile_definitions(pitlane_perf_shared PRIVATE PITLANE_RELAX_LATENCY=1)
    endif()
endif()

enable_testing()
foreach(area ${PITLANE_AREAS})
    add_test(NAME ${area} COMMAND pitlane_test_${area})
endforeach()
add_test(NAME perf COMMAND pitlane_perf)
set_tests_properties(perf PROPERTIES RUN_SERIAL TRUE)
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
    foreach(area ${PITLANE_AREAS})
        add_test(NAME ${area}_shared COMMAND pitlane_test_${area}_shared)
    endforeach()
    add_test(NAME perf_shared COMMAND pitlane_perf_shared)
    set_tests_properties(perf_shared PROPERTIES RUN_SERIAL TRUE)
endif()

install(TARGETS ${PITLANE_INSTALL_TARGETS}
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib)
install(DIRECTORY include/pitlane DESTINATION include)
