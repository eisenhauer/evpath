string(APPEND dashboard_cache "
BUILD_TESTING:BOOL=ON
")

if(POLICY CMP0057)
  cmake_policy(SET CMP0057 NEW)
endif()

if(NOT CTEST_BUILD_CONFIGURATION)
  set(CTEST_BUILD_CONFIGURATION Debug)
endif()

if(NOT DEFINED NCPUS)
  include(ProcessorCount)
  ProcessorCount(NCPUS)
endif()
math(EXPR N2CPUS "${NCPUS}*2")
if(NOT CTEST_BUILD_FLAGS)
  if(CTEST_CMAKE_GENERATOR STREQUAL "Unix Makefiles")
    set(CTEST_BUILD_FLAGS "-k -j${N2CPUS}")
  elseif(CTEST_CMAKE_GENERATOR STREQUAL "Ninja")
    set(CTEST_BUILD_FLAGS "-k0 -j${N2CPUS}")
  endif()
endif()
if(NOT PARALLEL_LEVEL IN_LIST CTEST_TEST_ARGS)
  list(APPEND CTEST_TEST_ARGS PARALLEL_LEVEL 1)
endif()

if(NOT dashboard_model)
  set(dashboard_model Experimental)
endif()
if(NOT dashboard_binary_name)
  set(dashboard_binary_name "build")
endif()
if(NOT dashboard_track)
  set(dashboard_track "Continuous Integration")
endif()
if(NOT "$ENV{CI_COMMIT_SHA}" STREQUAL "")
  set(CTEST_UPDATE_VERSION_OVERRIDE "$ENV{CI_COMMIT_SHA}")
  set(CTEST_UPDATE_VERSION_ONLY ON)
endif()
if(NOT "$ENV{CI_SITE_NAME}" STREQUAL "")
  set(CTEST_SITE "$ENV{CI_SITE_NAME}")
endif()
if(NOT "$ENV{CI_BUILD_NAME}" STREQUAL "")
  set(CTEST_BUILD_NAME "$ENV{CI_BUILD_NAME}")
endif()
if(NOT "$ENV{CI_ROOT_DIR}" STREQUAL "")
  set(CTEST_DASHBOARD_ROOT "$ENV{CI_ROOT_DIR}")
endif()
if(NOT "$ENV{CI_SOURCE_DIR}" STREQUAL "")
  set(CTEST_SOURCE_DIRECTORY "$ENV{CI_SOURCE_DIR}")
endif()
if(NOT "$ENV{CI_BIN_DIR}" STREQUAL "")
  set(CTEST_BINARY_DIRECTORY "$ENV{CI_BIN_DIR}")
endif()

find_program(CTEST_GIT_COMMAND git)
set(CTEST_UPDATE_COMMAND ${CTEST_GIT_COMMAND})
set(CTEST_UPDATE_TYPE git)

list(APPEND CTEST_UPDATE_NOTES_FILES "${CMAKE_CURRENT_LIST_FILE}")
include(${CMAKE_CURRENT_LIST_DIR}/../../dashboard/evpath_common.cmake)
if(ctest_build_num_warnings GREATER 0)
  message(FATAL_ERROR "Found ${ctest_build_num_warnings} warnings.")
endif()
