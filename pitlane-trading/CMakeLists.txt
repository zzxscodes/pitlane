cmake_minimum_required(VERSION 3.16)
project(pitlane-trading LANGUAGES CXX)

# pitlane-trading is both the paper/live console of the pitlane repository
# and a project that can be configured on its own:
#
#   cmake -S pitlane-trading -B build-trading
#   cmake --build build-trading -j
#
# A standalone configure resolves pitlane from PITLANE_SOURCE_DIR, then from a
# sibling source tree (../include/pitlane/pitlane.hpp), and finally fetches it
# from the upstream repository. An in-tree configure reuses the parent target.

if(TARGET pitlane)
    set(PITLANE_TRADING_IN_TREE ON)
    message(STATUS "pitlane-trading: using the pitlane target from the parent tree")
else()
    set(PITLANE_TRADING_STANDALONE ON)

    if(PITLANE_SOURCE_DIR)
        set(_pitlane_src "${PITLANE_SOURCE_DIR}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../include/pitlane/pitlane.hpp")
        get_filename_component(_pitlane_src "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    endif()

    # Keep a standalone configure to this project: the pitlane library is built
    # without its tests, shared library, C ABI, or this directory again.
    set(PITLANE_BUILD_TESTS OFF)
    set(PITLANE_BUILD_SHARED OFF)
    set(PITLANE_BUILD_CAPI OFF)
    set(PITLANE_BUILD_TRADING OFF)

    if(_pitlane_src)
        message(STATUS "pitlane-trading: using pitlane source at ${_pitlane_src}")
        add_subdirectory("${_pitlane_src}" "${CMAKE_BINARY_DIR}/_pitlane")
    else()
        include(FetchContent)
        set(PITLANE_FETCH_TAG "main" CACHE STRING "pitlane git branch or tag to fetch")
        message(STATUS "pitlane-trading: fetching pitlane (${PITLANE_FETCH_TAG})")
        FetchContent_Declare(pitlane
            GIT_REPOSITORY https://github.com/zzxscodes/pitlane.git
            GIT_TAG ${PITLANE_FETCH_TAG}
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(pitlane)
    endif()
endif()

include(CTest)

option(PITLANE_TRADING_CTP "Build the CTP market-data and trading gateway" ON)
option(PITLANE_TRADING_BUILD_TESTS "Register the pitlane-trading end-to-end tests" ON)
option(PITLANE_TRADING_BUILD_BENCH "Build the paper-center bench" ON)
option(PITLANE_TRADING_OPT "Optimise the console build" ON)
set(PITLANE_TRADING_CTP_API_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ctp"
    CACHE PATH "CTP API root (_se)")

if(DEFINED PITLANE_NATIVE)
    set(_pitlane_native ${PITLANE_NATIVE})
else()
    option(PITLANE_TRADING_NATIVE "Emit code for the build-host CPU (-march=native)" ON)
    set(_pitlane_native ${PITLANE_TRADING_NATIVE})
endif()

set(PITLANE_TRADING_FLAGS -Wall -Wextra -D_GNU_SOURCE)
if(PITLANE_TRADING_OPT)
    list(APPEND PITLANE_TRADING_FLAGS -O3 -DNDEBUG)
endif()
if(_pitlane_native)
    list(APPEND PITLANE_TRADING_FLAGS -march=native)
    # Cap auto-vectorization at 256 bits: AVX-512 can drop core frequency.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
        list(APPEND PITLANE_TRADING_FLAGS -mprefer-vector-width=256 -mno-avx512f)
    endif()
endif()

# The trading executable and the CTP adapter are the hot path, so they follow
# the core library's PGO mode. Generate and use must share one build tree:
# GCC looks for .gcda next to the objects that produced them.
if(NOT DEFINED PITLANE_PGO)
    set(PITLANE_PGO "OFF" CACHE STRING "Profile-guided optimization: OFF, GENERATE, or USE")
endif()
set(PITLANE_TRADING_PGO_LINK "")
if(PITLANE_PGO STREQUAL "GENERATE")
    list(APPEND PITLANE_TRADING_FLAGS -fprofile-generate)
    set(PITLANE_TRADING_PGO_LINK -fprofile-generate)
elseif(PITLANE_PGO STREQUAL "USE")
    list(APPEND PITLANE_TRADING_FLAGS -fprofile-use -fprofile-correction)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND PITLANE_TRADING_FLAGS -Wno-missing-profile)
    endif()
    set(PITLANE_TRADING_PGO_LINK -fprofile-use)
endif()

function(pitlane_trading_pgo target)
    if(PITLANE_TRADING_PGO_LINK)
        target_link_options(${target} PRIVATE ${PITLANE_TRADING_PGO_LINK})
    endif()
endfunction()

# Shared libraries for the CTP gateway load from the vendor directory at run time.
set(_PITLANE_CTP_MD_SO "${PITLANE_TRADING_CTP_API_ROOT}/lib/thostmduserapi_se.so")
set(_PITLANE_CTP_TD_SO "${PITLANE_TRADING_CTP_API_ROOT}/lib/thosttraderapi_se.so")
if(NOT EXISTS "${_PITLANE_CTP_MD_SO}")
    set(_PITLANE_CTP_MD_SO "${PITLANE_TRADING_CTP_API_ROOT}/lib/libthostmduserapi_se.so")
    set(_PITLANE_CTP_TD_SO "${PITLANE_TRADING_CTP_API_ROOT}/lib/libthosttraderapi_se.so")
endif()
set(PITLANE_TRADING_HAVE_CTP OFF)
if(PITLANE_TRADING_CTP)
    if(EXISTS "${PITLANE_TRADING_CTP_API_ROOT}/include/ThostFtdcMdApi.h" AND EXISTS "${_PITLANE_CTP_MD_SO}")
        set(PITLANE_TRADING_HAVE_CTP ON)
    else()
        message(WARNING "pitlane-trading: CTP API not found at ${PITLANE_TRADING_CTP_API_ROOT}; building without the live gateway")
    endif()
endif()

add_executable(pitlane_trading main.cpp adapters/ctp/md_engine.cpp adapters/ctp/td_client.cpp)
target_link_libraries(pitlane_trading PRIVATE pitlane)
target_include_directories(pitlane_trading PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(pitlane_trading PRIVATE
    PITLANE_CLIENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../client")
if(PITLANE_TRADING_HAVE_CTP)
    target_include_directories(pitlane_trading PRIVATE ${PITLANE_TRADING_CTP_API_ROOT}/include)
    # The vendor .so files carry no SONAME, so linking them by absolute path
    # would record that path as DT_NEEDED and pin the executable to this build
    # tree. Link the exact file names instead (-l:): DT_NEEDED keeps a plain
    # file name whatever prefix the SDK uses (thost*.so or libthost*.so), and
    # the RPATH below resolves it, which keeps the binary relocatable.
    get_filename_component(_PITLANE_CTP_MD_NAME "${_PITLANE_CTP_MD_SO}" NAME)
    get_filename_component(_PITLANE_CTP_TD_NAME "${_PITLANE_CTP_TD_SO}" NAME)
    target_link_directories(pitlane_trading PRIVATE "${PITLANE_TRADING_CTP_API_ROOT}/lib")
    target_link_libraries(pitlane_trading PRIVATE
        "-l:${_PITLANE_CTP_MD_NAME}" "-l:${_PITLANE_CTP_TD_NAME}")
    # Copy the vendor libraries next to the executable and keep only $ORIGIN in
    # the runpath, so no source or build path is embedded and the build tree can
    # be copied anywhere as a whole.
    add_custom_command(TARGET pitlane_trading POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_PITLANE_CTP_MD_SO}" "${_PITLANE_CTP_TD_SO}"
                "$<TARGET_FILE_DIR:pitlane_trading>")
    set_target_properties(pitlane_trading PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "\$ORIGIN")
    message(STATUS "pitlane-trading: CTP gateway enabled (${PITLANE_TRADING_CTP_API_ROOT})")
endif()
set_target_properties(pitlane_trading PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_compile_options(pitlane_trading PRIVATE ${PITLANE_TRADING_FLAGS})
pitlane_trading_pgo(pitlane_trading)

# Adapter checks: decode, book, fanout, and the console live view. No broker SDK.
if(PITLANE_TRADING_BUILD_TESTS AND BUILD_TESTING)
    add_executable(pitlane_trading_test_ctp tests/ctp_check.cpp)
    target_link_libraries(pitlane_trading_test_ctp PRIVATE pitlane)
    target_include_directories(pitlane_trading_test_ctp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(pitlane_trading_test_ctp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_compile_options(pitlane_trading_test_ctp PRIVATE ${PITLANE_TRADING_FLAGS})
    pitlane_trading_pgo(pitlane_trading_test_ctp)
    add_test(NAME trading_ctp COMMAND pitlane_trading_test_ctp)
    set_tests_properties(trading_ctp PROPERTIES RUN_SERIAL TRUE)
endif()

# Paper-center timings. No broker library, so this builds without the vendor SDK.
if(PITLANE_TRADING_BUILD_BENCH)
    add_executable(pitlane_trading_bench bench.cpp)
    target_link_libraries(pitlane_trading_bench PRIVATE pitlane)
    set_target_properties(pitlane_trading_bench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_compile_options(pitlane_trading_bench PRIVATE ${PITLANE_TRADING_FLAGS})
    pitlane_trading_pgo(pitlane_trading_bench)
endif()

# End-to-end checks: strategy -> backtest -> paper trading, no broker.
if(PITLANE_TRADING_BUILD_TESTS AND BUILD_TESTING AND TARGET pitlane_trading)
    # Run in the build tree so the tests never read a research.conf that a user
    # left in the source tree, and never overwrite the user's run/ directory.
    set(TRADING_CI_CONF "${CMAKE_CURRENT_SOURCE_DIR}/conf/paper.conf")
    add_test(NAME trading_paper COMMAND pitlane_trading --full ${TRADING_CI_CONF})
    add_test(NAME trading_backtest COMMAND pitlane_trading --backtest ${TRADING_CI_CONF})
    add_test(NAME trading_record COMMAND pitlane_trading --backtest --record run/tape_ci.csv ${TRADING_CI_CONF})
    add_test(NAME trading_research
        COMMAND pitlane_trading --research --tape run/tape_ci.csv --out run/research_ci.conf ${TRADING_CI_CONF})
    add_test(NAME trading_study_backtest
        COMMAND pitlane_trading --backtest --tape run/tape_ci.csv --study run/research_ci.conf ${TRADING_CI_CONF})
    set_tests_properties(trading_paper trading_backtest trading_record trading_research trading_study_backtest PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        RUN_SERIAL TRUE)
    set_tests_properties(trading_record PROPERTIES FIXTURES_SETUP tape_ci)
    set_tests_properties(trading_research PROPERTIES FIXTURES_REQUIRED tape_ci FIXTURES_SETUP study_ci)
    set_tests_properties(trading_study_backtest PROPERTIES FIXTURES_REQUIRED "tape_ci;study_ci")
endif()

if(PITLANE_TRADING_BUILD_TESTS AND BUILD_TESTING AND TARGET pitlane_trading_bench)
    add_test(NAME trading_bench COMMAND pitlane_trading_bench)
    set_tests_properties(trading_bench PROPERTIES RUN_SERIAL TRUE)
endif()
