# SPDX-FileCopyrightText: Copyright 2023 Mikhail Svetkin
# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

cmake_minimum_required(VERSION 3.25)

get_property(IN_TRY_COMPILE GLOBAL PROPERTY IN_TRY_COMPILE)

if(IN_TRY_COMPILE)
  return()
endif()

unset(IN_TRY_COMPILE)

include(${CMAKE_CURRENT_LIST_DIR}/bootstrap/vcpkg-config.cmake)

vcpkg_configure(
  CACHE_DIR_NAME tasks
  REPO https://github.com/microsoft/vcpkg.git
  REF c3867e714dd3a51c272826eea77267876517ed99 # release 2026.03.18
)

include($CACHE{_VCPKG_TOOLCHAIN_FILE})
