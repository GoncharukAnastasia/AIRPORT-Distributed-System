cmake_minimum_required(VERSION 3.20)
project(Airport)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
        httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)

add_executable(InformationPanel services/InformationPanel.cpp)
add_executable(GroundControl    services/GroundControl.cpp)
add_executable(FollowMe         services/FollowMe.cpp)
add_executable(Board         services/Board.cpp)

target_include_directories(InformationPanel PRIVATE ${CMAKE_SOURCE_DIR}/shared)
target_include_directories(GroundControl    PRIVATE ${CMAKE_SOURCE_DIR}/shared)
target_include_directories(FollowMe          PRIVATE ${CMAKE_SOURCE_DIR}/shared)
target_include_directories(Board          PRIVATE ${CMAKE_SOURCE_DIR}/shared)

target_link_libraries(InformationPanel PRIVATE
        httplib::httplib
        nlohmann_json::nlohmann_json
        PkgConfig::LIBPQXX
)
target_link_libraries(GroundControl PRIVATE httplib::httplib nlohmann_json::nlohmann_json)
target_link_libraries(Board PRIVATE httplib::httplib nlohmann_json::nlohmann_json)
target_link_libraries(FollowMe PRIVATE
        httplib::httplib
        nlohmann_json::nlohmann_json
        PkgConfig::LIBPQXX
)