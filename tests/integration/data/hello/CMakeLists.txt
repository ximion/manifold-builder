cmake_minimum_required(VERSION 3.16)
project(hello C)

set(HELLO_NAME hello-cmake CACHE STRING "Name of the installed program")
add_executable(${HELLO_NAME} hello.c)
install(TARGETS ${HELLO_NAME})
