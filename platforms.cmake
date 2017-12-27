# Copyright (c) 2018, Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

# We require cmake 3.2.0 or later
cmake_minimum_required(VERSION 3.2.0 FATAL_ERROR)

set(MAX_GEN 64)

macro(INIT_LIST LIST_TYPE ELEMENT_TYPE)
  foreach(IT RANGE 0 ${MAX_GEN} 1)
    list(APPEND ALL_${ELEMENT_TYPE}_${LIST_TYPE} " ")
  endforeach(IT)
endmacro(INIT_LIST)

macro(GET_LIST_FOR_GEN LIST_TYPE ELEMENT_TYPE GEN_NUMBER OUT_LIST)
  list(GET ALL_${ELEMENT_TYPE}_${LIST_TYPE} ${GEN_NUMBER} GEN_X_${LIST_TYPE})
  string(REPLACE "_" ";" ${OUT_LIST} ${GEN_X_${LIST_TYPE}})
endmacro(GET_LIST_FOR_GEN)

macro(ADD_ITEM_FOR_GEN LIST_TYPE ELEMENT_TYPE GEN_NUMBER ITEM)
  list(GET ALL_${ELEMENT_TYPE}_${LIST_TYPE} ${GEN_NUMBER} GEN_X_LIST)
  string(REPLACE " " "" GEN_X_LIST ${GEN_X_LIST})
  if("${GEN_X_LIST}" STREQUAL "")
    set(GEN_X_LIST "${ITEM}")
  else("${GEN_X_LIST}" STREQUAL "")
    set(GEN_X_LIST "${GEN_X_LIST}_${ITEM}")
  endif("${GEN_X_LIST}" STREQUAL "")
  list(REMOVE_AT ALL_${ELEMENT_TYPE}_${LIST_TYPE} ${GEN_NUMBER})
  list(INSERT ALL_${ELEMENT_TYPE}_${LIST_TYPE} ${GEN_NUMBER} ${GEN_X_LIST})
endmacro(ADD_ITEM_FOR_GEN)

macro(GEN_CONTAINS_PLATFORMS TYPE GEN_NUMBER OUT_FLAG)
  GET_LIST_FOR_GEN("PLATFORMS" ${TYPE} ${GEN_NUMBER} GEN_X_PLATFORMS)
  string(REPLACE " " "" GEN_X_PLATFORMS ${GEN_X_PLATFORMS})
  if("${GEN_X_PLATFORMS}" STREQUAL "")
    set(${OUT_FLAG} FALSE)
  else("${GEN_X_PLATFORMS}" STREQUAL "")
    set(${OUT_FLAG} TRUE)
  endif("${GEN_X_PLATFORMS}" STREQUAL "")
endmacro(GEN_CONTAINS_PLATFORMS)

macro(GET_AVAILABLE_PLATFORMS TYPE FLAG_NAME OUT_STR)
  set(${TYPE}_PLATFORM_LIST)
  set(${TYPE}_GEN_FLAGS_DEFINITONS)
  foreach(GEN_NUM RANGE ${MAX_GEN} 0 -1)
    GEN_CONTAINS_PLATFORMS(${TYPE} ${GEN_NUM} GENX_HAS_PLATFORMS)
    if(${GENX_HAS_PLATFORMS})
      list(APPEND ${TYPE}_GEN_FLAGS_DEFINITONS ${FLAG_NAME}_GEN${GEN_NUM})
      GET_LIST_FOR_GEN("PLATFORMS" ${TYPE} ${GEN_NUM} ${TYPE}_GENX_PLATFORMS)
      list(APPEND ${TYPE}_PLATFORM_LIST ${${TYPE}_GENX_PLATFORMS})
      if(NOT DEFAULT_${TYPE}_PLATFORM)
        list(GET ${TYPE}_PLATFORM_LIST 0 DEFAULT_${TYPE}_PLATFORM ${PLATFORM_IT})
      endif()
      if(NOT DEFAULT_${TYPE}_GEN${GEN_NUM}_PLATFORM)
        list(GET ${TYPE}_GENX_PLATFORMS 0 DEFAULT_${TYPE}_GEN${GEN_NUM}_PLATFORM)
      endif()
    endif()
  endforeach()
  foreach(PLATFORM_IT ${${TYPE}_PLATFORM_LIST})
    set(${OUT_STR} "${${OUT_STR}} ${PLATFORM_IT}")
    list(APPEND ${TYPE}_GEN_FLAGS_DEFINITONS ${FLAG_NAME}_${PLATFORM_IT})
  endforeach()
endmacro(GET_AVAILABLE_PLATFORMS)

macro(GET_PLATFORMS_FOR_GEN TYPE GEN_NUMBER OUT_LIST)
  GET_LIST_FOR_GEN("PLATFORMS" ${TYPE} ${GEN_NUMBER} ${OUT_LIST})
endmacro(GET_PLATFORMS_FOR_GEN)

macro(GET_TEST_CONFIGURATIONS_FOR_PLATFORM TYPE GEN_NUMBER PLATFORM OUT_LIST)
  set(${OUT_LIST})
  string(TOLOWER ${PLATFORM} PLATFORM_LOWER)
  GET_LIST_FOR_GEN("CONFIGURATIONS" ${TYPE} ${GEN_NUMBER} ALL_CONFIGURATIONS_FOR_GEN)
  foreach(CONFIGURATION ${ALL_CONFIGURATIONS_FOR_GEN})
    string(REPLACE "/" ";" CONFIGURATION_PARAMS ${CONFIGURATION})
    list(GET CONFIGURATION_PARAMS 0 CONFIGURATION_PLATFORM)
    if(${CONFIGURATION_PLATFORM} STREQUAL ${PLATFORM_LOWER})
      list(APPEND ${OUT_LIST} ${CONFIGURATION})
    endif()
  endforeach(CONFIGURATION)
endmacro(GET_TEST_CONFIGURATIONS_FOR_PLATFORM)

macro(PLATFORM_HAS_2_0 GEN_NUMBER PLATFORM_NAME OUT_FLAG)
  GET_LIST_FOR_GEN("PLATFORMS" "SUPPORTED_2_0" ${GEN_NUMBER} GEN_X_PLATFORMS)
  list(FIND GEN_X_PLATFORMS ${PLATFORM_NAME} PLATFORM_EXISTS)
  if("${PLATFORM_EXISTS}" LESS 0)
    set(${OUT_FLAG} FALSE)
  else("${PLATFORM_EXISTS}" LESS 0)
    set(${OUT_FLAG} TRUE)
  endif("${PLATFORM_EXISTS}" LESS 0)

endmacro(PLATFORM_HAS_2_0 PLATFORM_NAME OUT_FLAG)

macro(PLATFORM_TESTED_WITH_APPVERIFIER GEN_NUMBER PLATFORM_NAME OUT_FLAG)
  GET_LIST_FOR_GEN("PLATFORMS" "TESTED_APPVERIFIER" ${GEN_NUMBER} GEN_X_PLATFORMS)
  list(FIND GEN_X_PLATFORMS ${PLATFORM_NAME} PLATFORM_EXISTS)
  if("${PLATFORM_EXISTS}" LESS 0)
    set(${OUT_FLAG} FALSE)
  else("${PLATFORM_EXISTS}" LESS 0)
    set(${OUT_FLAG} TRUE)
  endif("${PLATFORM_EXISTS}" LESS 0)

endmacro(PLATFORM_TESTED_WITH_APPVERIFIER PLATFORM_NAME OUT_FLAG)

# default flag for GenX devices support
set(SUPPORT_GEN_DEFAULT TRUE CACHE BOOL "default value for SUPPORT_GENx")
# default flag for platform support
set(SUPPORT_PLATFORM_DEFAULT TRUE CACHE BOOL "default value for support platform")

# Define the hardware configurations we support
set(SUPPORT_GEN8 ${SUPPORT_GEN_DEFAULT} CACHE BOOL "Support Gen8 devices")
set(SUPPORT_GEN9 ${SUPPORT_GEN_DEFAULT} CACHE BOOL "Support Gen9 devices")

# Define the hardware configurations we test
set(TESTS_GEN8 ${SUPPORT_GEN8} CACHE BOOL "Build ULTs for Gen8 devices")
set(TESTS_GEN9 ${SUPPORT_GEN9} CACHE BOOL "Build ULTs for Gen9 devices")

if(SUPPORT_GEN9)
  set(SUPPORT_SKL ${SUPPORT_PLATFORM_DEFAULT} CACHE BOOL "Support SKL")
  set(SUPPORT_KBL ${SUPPORT_PLATFORM_DEFAULT} CACHE BOOL "Support KBL")
  set(SUPPORT_BXT ${SUPPORT_PLATFORM_DEFAULT} CACHE BOOL "Support BXT")
  set(SUPPORT_GLK ${SUPPORT_PLATFORM_DEFAULT} CACHE BOOL "Support GLK")
  set(SUPPORT_CFL ${SUPPORT_PLATFORM_DEFAULT} CACHE BOOL "Support CFL")
endif()

if(TESTS_GEN9)
  if(SUPPORT_SKL)
    set(TESTS_SKL ${TESTS_GEN9} CACHE BOOL "Build ULTs for SKL")
  endif()
  if(SUPPORT_KBL)
    set(TESTS_KBL ${TESTS_GEN9} CACHE BOOL "Build ULTs for KBL")
  endif()
  if(SUPPORT_GLK)
    set(TESTS_GLK ${TESTS_GEN9} CACHE BOOL "Build ULTs for GLK")
  endif()
  if(SUPPORT_BXT)
    set(TESTS_BXT ${TESTS_GEN9} CACHE BOOL "Build ULTs for BXT")
  endif()
  if(SUPPORT_CFL)
    set(TESTS_CFL ${TESTS_GEN9} CACHE BOOL "Build ULTs for CFL")
  endif()
endif()

# Init lists
INIT_LIST("FAMILY_NAME" "TESTED")
INIT_LIST("PLATFORMS" "SUPPORTED")
INIT_LIST("PLATFORMS" "SUPPORTED_2_0")
INIT_LIST("PLATFORMS" "TESTED")
INIT_LIST("PLATFORMS" "TESTED_APPVERIFIER")
INIT_LIST("CONFIGURATIONS" "UNIT_TESTS")
INIT_LIST("CONFIGURATIONS" "AUB_TESTS")
INIT_LIST("CONFIGURATIONS" "MT_TESTS")

# Add supported and tested platforms
if(SUPPORT_GEN8)
  ADD_ITEM_FOR_GEN("PLATFORMS" "SUPPORTED" 8 "BDW")
  ADD_ITEM_FOR_GEN("PLATFORMS" "SUPPORTED_2_0" 8 "BDW")
  if(TESTS_GEN8)
    ADD_ITEM_FOR_GEN("FAMILY_NAME" "TESTED" 8 "BDWFamily")
    ADD_ITEM_FOR_GEN("PLATFORMS" "TESTED" 8 "BDW")
    ADD_ITEM_FOR_GEN("PLATFORMS" "TESTED_APPVERIFIER" 8 "BDW")
    ADD_ITEM_FOR_GEN("CONFIGURATIONS" "AUB_TESTS" 8 "bdw/1/3/8")
    ADD_ITEM_FOR_GEN("CONFIGURATIONS" "MT_TESTS" 8 "bdw/1/3/8")
    ADD_ITEM_FOR_GEN("CONFIGURATIONS" "UNIT_TESTS" 8 "bdw/1/3/8")
  endif()
endif(SUPPORT_GEN8)

if(SUPPORT_GEN9)
  if(TESTS_GEN9)
    ADD_ITEM_FOR_GEN("FAMILY_NAME" "TESTED" 9 "SKLFamily")
  endif()
  if(SUPPORT_SKL)
    ADD_ITEM_FOR_GEN("PLATFORMS" "SUPPORTED" 9 "SKL")
    ADD_ITEM_FOR_GEN("PLATFORMS" "SUPPORTED_2_0" 9 "SKL")
    if(TESTS_SKL)
      ADD_ITEM_FOR_GEN("PLATFORMS" "TESTED"  9 "SKL")
      ADD_ITEM_FOR_GEN("PLATFORMS" "TESTED_APPVERIFIER"  9 "SKL")
      ADD_ITEM_FOR_GEN("CONFIGURATIONS" "AUB_TESTS" 9 "skl/1/3/8")
      ADD_ITEM_FOR_GEN("CONFIGURATIONS" "MT_TESTS" 9 "skl/1/3/8")
      ADD_ITEM_FOR_GEN("CONFIGURATIONS" "UNIT_TESTS" 9 "skl/1/3/8")
    endif()
  endif()

  if(SUPPORT_KBL)
    ADD_ITEM_FOR_GEN("PLATFORMS" "SUPPORTED" 9 "KBL")
    ADD_ITEM_FOR_GEN("PLATFORMS" "SUPPORTED_2_0" 9 "KBL")
    if(TESTS_KBL)
      ADD_ITEM_FOR_GEN("PLATFORMS" "TESTED"  9 "KBL")
      ADD_ITEM_FOR_GEN("CONFIGURATIONS" "UNIT_TESTS" 9 "kbl/1/3/6")
    endif()
  endif()

  if(SUPPORT_GLK)
    ADD_ITEM_FOR_GEN("PLATFORMS" "SUPPORTED" 9 "GLK")
    if(TESTS_GLK)
      ADD_ITEM_FOR_GEN("PLATFORMS" "TESTED"  9 "GLK")
      ADD_ITEM_FOR_GEN("CONFIGURATIONS" "UNIT_TESTS" 9 "glk/1/3/6")
    endif()
  endif()

  if(SUPPORT_CFL)
    ADD_ITEM_FOR_GEN("PLATFORMS" "SUPPORTED" 9 "CFL")
    ADD_ITEM_FOR_GEN("PLATFORMS" "SUPPORTED_2_0" 9 "CFL")
    if(TESTS_CFL)
      ADD_ITEM_FOR_GEN("PLATFORMS" "TESTED"  9 "CFL")
      ADD_ITEM_FOR_GEN("CONFIGURATIONS" "UNIT_TESTS" 9 "cfl/1/3/6")
    endif()
  endif()

  if(SUPPORT_BXT)
    ADD_ITEM_FOR_GEN("PLATFORMS" "SUPPORTED" 9 "BXT")
    if(TESTS_BXT)
      ADD_ITEM_FOR_GEN("PLATFORMS" "TESTED"  9 "BXT")
      ADD_ITEM_FOR_GEN("CONFIGURATIONS" "AUB_TESTS" 9 "bxt/1/3/6")
      ADD_ITEM_FOR_GEN("CONFIGURATIONS" "UNIT_TESTS" 9 "bxt/1/3/6")
    endif()
  endif()
endif(SUPPORT_GEN9)

# Get platform lists, flag definition and set default platforms
GET_AVAILABLE_PLATFORMS("SUPPORTED" "SUPPORT" ALL_AVAILABLE_SUPPORTED_PLATFORMS)
GET_AVAILABLE_PLATFORMS("TESTED" "TESTS" ALL_AVAILABLE_TESTED_PLATFORMS)

message(STATUS "All supported platforms: ${ALL_AVAILABLE_SUPPORTED_PLATFORMS}")
message(STATUS "All tested platforms: ${ALL_AVAILABLE_TESTED_PLATFORMS}")

message(STATUS "Default supported platform: ${DEFAULT_SUPPORTED_PLATFORM}")

list(FIND SUPPORTED_PLATFORM_LIST ${DEFAULT_SUPPORTED_PLATFORM} VALID_DEFAULT_SUPPORTED_PLATFORM)
if(VALID_DEFAULT_SUPPORTED_PLATFORM LESS 0)
  message(FATAL_ERROR "Not a valid supported platform: ${DEFAULT_SUPPORTED_PLATFORM}")
endif()

message(STATUS "Default tested platform: ${DEFAULT_TESTED_PLATFORM}")

if(DEFAULT_TESTED_PLATFORM)
  list(FIND TESTED_PLATFORM_LIST ${DEFAULT_TESTED_PLATFORM} VALID_DEFAULT_TESTED_PLATFORM)
  if(VALID_DEFAULT_TESTED_PLATFORM LESS 0)
    message(FATAL_ERROR "Not a valid tested platform: ${DEFAULT_TESTED_PLATFORM}")
  endif()
endif()

if(NOT DEFAULT_TESTED_FAMILY_NAME)
  foreach(GEN_NUM RANGE ${MAX_GEN} 0 -1)
    list(GET ALL_TESTED_FAMILY_NAME ${GEN_NUM} GEN_FAMILY_NAME)
    if(NOT GEN_FAMILY_NAME STREQUAL " ")
      set(DEFAULT_TESTED_FAMILY_NAME ${GEN_FAMILY_NAME})
      break()
    endif()
  endforeach()
endif()
message(STATUS "Default tested family name: ${DEFAULT_TESTED_FAMILY_NAME}")
