cmake_minimum_required(VERSION 3.16)

# No usar EXTRA_COMPONENT_DIRS ni mqtt aquí.
# Este Gateway Thread solo usa OpenThread + UART.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(thread_gateway_uart)
