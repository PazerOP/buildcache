#---------------------------------------------------------------------------------------------------
# Copyright (c) 2018 Marcus Geelnard
#
# This software is provided 'as-is', without any express or implied warranty. In no event will the
# authors be held liable for any damages arising from the use of this software.
#
# Permission is granted to anyone to use this software for any purpose, including commercial
# applications, and to alter it and redistribute it freely, subject to the following restrictions:
#
#  1. The origin of this software must not be misrepresented; you must not claim that you wrote
#     the original software. If you use this software in a product, an acknowledgment in the
#     product documentation would be appreciated but is not required.
#
#  2. Altered source versions must be plainly marked as such, and must not be misrepresented as
#     being the original software.
#
#  3. This notice may not be removed or altered from any source distribution.
#---------------------------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)
project(BuildCache)

# We build everything against the C++14 standard.
set(CMAKE_CXX_STANDARD 17)
set(CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# For tooling (e.g. clang-tidy).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# For tests.
enable_testing()
include(cmake/buildcache_add_test.cmake)

if(MINGW)
  # For ease of deployment, statically link the standard libraries when using MinGW.
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++")

  # We target Windows Vista or higher.
  add_definitions(-D_WIN32_WINNT=0x0600)
endif()

if(MSVC)
  # Avoid CRT secure warnings when using MSVC.
  add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

# Thrid party libraries used by BuildCache.
add_subdirectory(third_party)

# For the project internal files, all includes are relative to the root folder.
include_directories(.)

# BuildCache internal libraries.
add_subdirectory(base)
add_subdirectory(config)
add_subdirectory(sys)
add_subdirectory(cache)
add_subdirectory(wrappers)

# Sources for the buildcache application.
set(BUILDCACHE_SRCS
    main.cpp)

# On Windows we want to include an application resource (for version information).
if(WIN32 OR MINGW)
  list(APPEND BUILDCACHE_SRCS buildcache.rc)
  if(MINGW AND NOT CMAKE_RC_COMPILER)
    # This is an ugly hack to find and use the MinGW resource compiler.
    string(REGEX REPLACE "g\\+\\+$" "windres" CMAKE_RC_COMPILER "${CMAKE_CXX_COMPILER}")
    if(EXISTS "${CMAKE_RC_COMPILER}")
      enable_language(RC)
      set(CMAKE_RC_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR} -O COFF")
      set(CMAKE_RC_COMPILE_OBJECT "${CMAKE_RC_COMPILER} ${CMAKE_RC_FLAGS} -i <SOURCE> -o <OBJECT>")
    endif()
  endif()
endif()

# The BuildCache application.
add_executable(buildcache ${BUILDCACHE_SRCS})
target_link_libraries(buildcache base config sys cache wrappers)

# Doxygen documentation.
find_package(Doxygen)
if(DOXYGEN_FOUND AND NOT (${CMAKE_VERSION} VERSION_LESS "3.9.0"))
  set(DOXYGEN_PROJECT_NAME BuildCache)
  set(DOXYGEN_OUTPUT_DIRECTORY doc)
  set(DOXYGEN_GENERATE_HTML YES)
  set(DOXYGEN_GENERATE_LATEX NO)
  set(DOXYGEN_EXCLUDE_PATTERNS "*/third_party/*")
  set(DOXYGEN_EXTRACT_PRIVATE YES)
  set(DOXYGEN_EXTRACT_STATIC YES)
  set(DOXYGEN_QUIET YES)
  set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)
  doxygen_add_docs(doc
                   ${PROJECT_SOURCE_DIR}
                   COMMENT "Generating documentation with Doxygen")
endif()

# Installation.
install(TARGETS buildcache)
install(DIRECTORY ../lua-examples
        TYPE DATA)

# This ensures that required MSVC runtime libraries are installed.
include(InstallRequiredSystemLibraries)
