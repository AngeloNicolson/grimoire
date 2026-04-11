cmake_minimum_required(VERSION 3.14)
project(grimoire VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSESW REQUIRED ncursesw)

add_executable(grimoire
  src/main.cpp
)

target_include_directories(grimoire PRIVATE ${NCURSESW_INCLUDE_DIRS})
target_link_libraries(grimoire PRIVATE ${NCURSESW_LIBRARIES} nlohmann_json::nlohmann_json)

install(TARGETS grimoire DESTINATION bin)
