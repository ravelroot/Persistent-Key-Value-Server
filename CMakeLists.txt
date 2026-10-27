cmake_minimum_required(VERSION 3.20)

project(Arca VERSION 1.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ARCA_BUILD_TESTS "Compila a suíte de testes" ON)
option(ARCA_WERROR "Trata avisos como erros" OFF)
set(ARCA_SANITIZE "" CACHE STRING "address, thread ou undefined")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive- /EHsc /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS)
  if(ARCA_WERROR)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(ARCA_WERROR)
    add_compile_options(-Werror)
  endif()
  if(ARCA_SANITIZE)
    add_compile_options(-fsanitize=${ARCA_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${ARCA_SANITIZE})
  endif()
endif()

file(GLOB ARCA_ENGINE_SOURCES CONFIGURE_DEPENDS
  src/util/*.cpp
  src/db/*.cpp
  src/table/*.cpp)

add_library(arca_engine STATIC ${ARCA_ENGINE_SOURCES})
target_include_directories(arca_engine PUBLIC include PRIVATE src)
target_include_directories(arca_engine INTERFACE src)
target_link_libraries(arca_engine PUBLIC Threads::Threads)

file(GLOB ARCA_SQL_SOURCES CONFIGURE_DEPENDS src/sql/*.cpp)
add_library(arca_sql STATIC ${ARCA_SQL_SOURCES})
target_link_libraries(arca_sql PUBLIC arca_engine)

file(GLOB ARCA_NET_SOURCES CONFIGURE_DEPENDS src/net/*.cpp)
add_library(arca_net STATIC ${ARCA_NET_SOURCES})
target_link_libraries(arca_net PUBLIC arca_engine)
if(WIN32)
  target_link_libraries(arca_net PUBLIC ws2_32)
endif()

file(GLOB ARCA_SERVER_SOURCES CONFIGURE_DEPENDS src/server/*.cpp)
add_library(arca_server STATIC ${ARCA_SERVER_SOURCES})
target_link_libraries(arca_server PUBLIC arca_sql arca_net)

add_executable(arca-server tools/arca_server.cpp)
target_link_libraries(arca-server PRIVATE arca_server)

add_executable(arca-cli tools/arca_cli.cpp)
target_link_libraries(arca-cli PRIVATE arca_net)

add_executable(arca-bench tools/arca_bench.cpp)
target_link_libraries(arca-bench PRIVATE arca_net)

add_executable(arca-inspect tools/arca_inspect.cpp)
target_link_libraries(arca-inspect PRIVATE arca_engine)

add_executable(arca-embedded examples/embedded.cpp)
target_link_libraries(arca-embedded PRIVATE arca_engine)

if(ARCA_BUILD_TESTS)
  enable_testing()
  file(GLOB ARCA_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
  add_executable(arca-tests ${ARCA_TEST_SOURCES})
  target_link_libraries(arca-tests PRIVATE arca_server)
  add_test(NAME arca-tests COMMAND arca-tests)
endif()

install(TARGETS arca-server arca-cli arca-bench arca-inspect RUNTIME DESTINATION bin)
