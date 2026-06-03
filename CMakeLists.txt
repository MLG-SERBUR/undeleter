cmake_minimum_required(VERSION 3.15)
project(undeleter-bot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find D++ library
find_package(DPP REQUIRED)

# If D++ is not found via find_package, try linking manually
# You may need to install D++ first: https://dpp.dev
# On Linux: sudo apt install libdpp-dev (if available) or build from source

add_executable(undeleter-bot src/main.cpp)

target_link_libraries(undeleter-bot PRIVATE dpp)

# For OpenSSL (required by D++)
find_package(OpenSSL REQUIRED)
target_link_libraries(undeleter-bot PRIVATE OpenSSL::SSL)

# For zlib (required by D++)
find_package(ZLIB REQUIRED)
target_link_libraries(undeleter-bot PRIVATE ZLIB::ZLIB)

# Install targets
install(TARGETS undeleter-bot DESTINATION bin)
