cmake_minimum_required(VERSION 3.16)

project(calendly LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(calendly_core STATIC
  src/calendly.cpp
  src/notifier.cpp
  src/paths.cpp
)
target_include_directories(calendly_core PUBLIC include)

add_executable(calendly src/main.cpp)
target_link_libraries(calendly PRIVATE calendly_core)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(calendly_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(calendly PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(calendly_tests tests/test_calendly.cpp)
  target_link_libraries(calendly_tests PRIVATE calendly_core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(calendly_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME calendly_tests COMMAND calendly_tests)
endif()

install(TARGETS calendly RUNTIME DESTINATION bin)
