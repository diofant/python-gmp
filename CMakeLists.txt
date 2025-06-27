cmake_minimum_required(VERSION 3.15...3.31)
project(${SKBUILD_PROJECT_NAME} LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH ".")

find_package(GMPLIB REQUIRED)

find_package(
  Python
  COMPONENTS Development.Module
  REQUIRED)

python_add_library(gmp MODULE fmt.c main.c zz.c WITH_SOABI)
target_link_libraries(gmp PRIVATE -lgmp)

install(TARGETS gmp DESTINATION .)
