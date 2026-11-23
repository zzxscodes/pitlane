cmake_minimum_required(VERSION 3.16)
project(pitlane VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PITLANE_SOURCES
    src/desk/broker.cpp
    src/desk/cash.cpp
    src/desk/codec.cpp
    src/desk/config.cpp
    src/desk/feed.cpp
    src/desk/logline.cpp
    src/desk/names.cpp
    src/desk/session.cpp
    src/desk/region.cpp
    src/desk/runtime.cpp
    src/desk/signal.cpp
    src/desk/store.cpp
    src/desk/tape_rec.cpp
    src/desk/yard.cpp
    src/book/calendar.cpp
    src/book/cross.cpp
    src/book/venue.cpp
    src/order/lots.cpp
    src/ops/host.cpp
    src/ops/http.cpp
    src/time/simclock.cpp
    src/util/textutil.cpp
    src/util/wirelog.cpp)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# Objects are position-independent, so libpitlane.a can also be linked into another shared library.
option(PITLANE_BUILD_SHARED "Build libpitlane.so in addition to the static library" ON)
option(PITLANE_BUILD_TESTS "Build area tests and the latency program" ON)
option(PITLANE_BUILD_TRADING "Build the pitlane-trading trading console subproject" ON)
option(PITLANE_BUILD_CAPI "Build the C ABI shared library" ON)
option(PITLANE_BUILD_PYTHON "Register the optional Python ctypes smoke test" OFF)
option(PITLANE_NATIVE "Emit code for the build-host CPU (-march=native)" ON)
option(PITLANE_LTO "Enable link-time optimization across translation units" ON)
option(PITLANE_SANITIZE "Build with address and undefined-behavior sanitizers" OFF)
option(PITLANE_RELAX_LATENCY "Widen tick-to-trade latency bounds in perf (CI / busy hosts)" OFF)
set(PITLANE_PGO "OFF" CACHE STRING "Profile-guided optimization: OFF, GENERATE, or USE")
set_property(CACHE PITLANE_PGO PROPERTY STRINGS OFF GENERATE USE)

include(CheckIPOSupported)
check_ipo_supported(RESULT PITLANE_IPO_OK OUTPUT PITLANE_IPO_MSG)
find_package(Threads REQUIRED)

set(PITLANE_WARN_FLAGS -Wall -Wextra -Wpedantic -Werror)
# Let the compiler inline and clone across the shared-library boundary; the
# exported C ABI stays default-visible, and the library does not rely on ELF
# symbol interposition for customization.
set(PITLANE_OPT_FLAGS -O3 -fno-exceptions -fno-rtti -fno-semantic-interposition -D_GNU_SOURCE)
if(PITLANE_NATIVE)
    list(APPEND PITLANE_OPT_FLAGS -march=native)
    # Cap auto-vectorization at 256 bits: AVX-512 can drop core frequency.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
        list(APPEND PITLANE_OPT_FLAGS -mprefer-vector-width=256 -mno-avx512f)
    endif()
endif()
if(PITLANE_PGO STREQUAL "GENERATE")
    list(APPEND PITLANE_OPT_FLAGS -fprofile-generate)
    set(PITLANE_PGO_LINK -fprofile-generate)
elseif(PITLANE_PGO STREQUAL "USE")
    list(APPEND PITLANE_OPT_FLAGS -fprofile-use -fprofile-correction)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # Tests and rarely used paths may not appear in the training run.
        list(APPEND PITLANE_OPT_FLAGS -Wno-missing-profile)
    endif()
    set(PITLANE_PGO_LINK -fprofile-use)
else()
    set(PITLANE_PGO_LINK "")
endif()

function(pitlane_apply target)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
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
add_library(pitlane::pitlane ALIAS pitlane)
pitlane_apply(pitlane)
pitlane_lto(pitlane)

set(PITLANE_INSTALL_TARGETS pitlane)
if(PITLANE_BUILD_SHARED)
    add_library(pitlane_shared SHARED ${PITLANE_SOURCES})
    add_library(pitlane::pitlane_shared ALIAS pitlane_shared)
    pitlane_apply(pitlane_shared)
    pitlane_lto(pitlane_shared)
    set_target_properties(pitlane_shared PROPERTIES
        OUTPUT_NAME pitlane
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
        EXPORT_NAME pitlane_shared)
    list(APPEND PITLANE_INSTALL_TARGETS pitlane_shared)
endif()

set(PITLANE_AREAS desk book match order time study strat util host net oms factor alloc ops)
set(PITLANE_SAN_TARGETS pitlane)

# Optional C ABI shared library for ctypes / FFI callers.
if(PITLANE_BUILD_CAPI)
    add_library(pitlane_c SHARED src/capi/pitlane_c.cpp)
    pitlane_apply(pitlane_c)
    target_link_libraries(pitlane_c PRIVATE pitlane)
    target_compile_definitions(pitlane_c PRIVATE PITLANE_VERSION_STR="${PROJECT_VERSION}")
    set_target_properties(pitlane_c PROPERTIES OUTPUT_NAME pitlane_c)
    pitlane_lto(pitlane_c)
    list(APPEND PITLANE_INSTALL_TARGETS pitlane_c)
    list(APPEND PITLANE_SAN_TARGETS pitlane_c)
endif()

# The trading console is its own project. It builds here as a subdirectory and
# can also be configured on its own from pitlane-trading/.
if(PITLANE_BUILD_TRADING AND NOT PITLANE_TRADING_STANDALONE)
    if(NOT DEFINED PITLANE_TRADING_BUILD_TESTS)
        set(PITLANE_TRADING_BUILD_TESTS ${PITLANE_BUILD_TESTS})
    endif()
    add_subdirectory(pitlane-trading)
    if(TARGET pitlane_trading)
        list(APPEND PITLANE_SAN_TARGETS pitlane_trading)
    endif()
    if(TARGET pitlane_trading_bench)
        list(APPEND PITLANE_SAN_TARGETS pitlane_trading_bench)
    endif()
endif()

if(PITLANE_BUILD_TESTS AND BUILD_TESTING)
    foreach(area ${PITLANE_AREAS})
        add_executable(pitlane_test_${area} tests/${area}/check.cpp)
        target_link_libraries(pitlane_test_${area} PRIVATE pitlane)
        target_compile_options(pitlane_test_${area} PRIVATE ${PITLANE_WARN_FLAGS} ${PITLANE_OPT_FLAGS})
        if(PITLANE_PGO_LINK)
            target_link_options(pitlane_test_${area} PRIVATE ${PITLANE_PGO_LINK})
        endif()
        pitlane_lto(pitlane_test_${area})
        add_test(NAME ${area} COMMAND pitlane_test_${area})
        list(APPEND PITLANE_SAN_TARGETS pitlane_test_${area})
    endforeach()

    add_executable(pitlane_perf tests/time/perf.cpp)
    target_link_libraries(pitlane_perf PRIVATE pitlane)
    target_include_directories(pitlane_perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pitlane-trading)
    target_compile_options(pitlane_perf PRIVATE ${PITLANE_WARN_FLAGS} ${PITLANE_OPT_FLAGS})
    if(PITLANE_PGO_LINK)
        target_link_options(pitlane_perf PRIVATE ${PITLANE_PGO_LINK})
    endif()
    pitlane_lto(pitlane_perf)
    add_test(NAME perf COMMAND pitlane_perf)
    set_tests_properties(perf PROPERTIES RUN_SERIAL TRUE)
    list(APPEND PITLANE_SAN_TARGETS pitlane_perf)

    if(PITLANE_BUILD_SHARED)
        foreach(area ${PITLANE_AREAS})
            add_executable(pitlane_test_${area}_shared tests/${area}/check.cpp)
            target_link_libraries(pitlane_test_${area}_shared PRIVATE pitlane_shared)
            target_compile_options(pitlane_test_${area}_shared PRIVATE ${PITLANE_WARN_FLAGS} ${PITLANE_OPT_FLAGS})
            if(PITLANE_PGO_LINK)
                target_link_options(pitlane_test_${area}_shared PRIVATE ${PITLANE_PGO_LINK})
            endif()
            pitlane_lto(pitlane_test_${area}_shared)
            add_test(NAME ${area}_shared COMMAND pitlane_test_${area}_shared)
            list(APPEND PITLANE_SAN_TARGETS pitlane_test_${area}_shared)
        endforeach()

        add_executable(pitlane_perf_shared tests/time/perf.cpp)
        target_link_libraries(pitlane_perf_shared PRIVATE pitlane_shared)
        target_include_directories(pitlane_perf_shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pitlane-trading)
        target_compile_options(pitlane_perf_shared PRIVATE ${PITLANE_WARN_FLAGS} ${PITLANE_OPT_FLAGS})
        if(PITLANE_PGO_LINK)
            target_link_options(pitlane_perf_shared PRIVATE ${PITLANE_PGO_LINK})
        endif()
        pitlane_lto(pitlane_perf_shared)
        add_test(NAME perf_shared COMMAND pitlane_perf_shared)
        set_tests_properties(perf_shared PROPERTIES RUN_SERIAL TRUE)
        list(APPEND PITLANE_SAN_TARGETS pitlane_perf_shared)
    endif()

    if(PITLANE_BUILD_CAPI AND TARGET pitlane_c)
        add_executable(pitlane_test_capi tests/capi/check.cpp)
        target_link_libraries(pitlane_test_capi PRIVATE pitlane pitlane_c)
        target_compile_options(pitlane_test_capi PRIVATE ${PITLANE_WARN_FLAGS} ${PITLANE_OPT_FLAGS})
        if(PITLANE_PGO_LINK)
            target_link_options(pitlane_test_capi PRIVATE ${PITLANE_PGO_LINK})
        endif()
        pitlane_lto(pitlane_test_capi)
        add_test(NAME capi COMMAND pitlane_test_capi)
        list(APPEND PITLANE_SAN_TARGETS pitlane_test_capi)

        if(PITLANE_BUILD_PYTHON)
            find_package(Python3 COMPONENTS Interpreter)
            if(Python3_Interpreter_FOUND)
                add_test(NAME python_smoke
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/python/tests/test_smoke.py)
                set_tests_properties(python_smoke PROPERTIES
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    ENVIRONMENT "PYTHONPATH=python:$<TARGET_FILE_DIR:pitlane_c>"
                    REQUIRED_FILES "$<TARGET_FILE:pitlane_c>")
            endif()
        endif()
    endif()
endif()

if(PITLANE_SANITIZE)
    if(PITLANE_BUILD_SHARED)
        list(APPEND PITLANE_SAN_TARGETS pitlane_shared)
    endif()
    foreach(tgt ${PITLANE_SAN_TARGETS})
        if(TARGET ${tgt})
            target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -O1)
            target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
            set_property(TARGET ${tgt} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
        endif()
    endforeach()
endif()

if(PITLANE_RELAX_LATENCY OR PITLANE_SANITIZE)
    if(TARGET pitlane_perf)
        target_compile_definitions(pitlane_perf PRIVATE PITLANE_RELAX_LATENCY=1)
    endif()
    if(TARGET pitlane_perf_shared)
        target_compile_definitions(pitlane_perf_shared PRIVATE PITLANE_RELAX_LATENCY=1)
    endif()
endif()

set_target_properties(pitlane PROPERTIES EXPORT_NAME pitlane)

install(TARGETS ${PITLANE_INSTALL_TARGETS}
    EXPORT pitlaneTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/pitlane DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT pitlaneTargets
    FILE pitlaneTargets.cmake
    NAMESPACE pitlane::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pitlane)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pitlaneConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/pitlaneConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pitlane)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/pitlaneConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/pitlaneConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/pitlaneConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pitlane)

install(FILES LICENSE README.md README_CN.md CHANGELOG.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY doc/site/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/site
    OPTIONAL)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/doc/HOST_TUNING.md")
    install(FILES doc/HOST_TUNING.md doc/HOST_TUNING_CN.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        OPTIONAL)
endif()
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/pitlane.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/pitlane.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pitlane.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
