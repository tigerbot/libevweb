cmake_minimum_required(VERSION 2.6)
project(EVWEB C)
include_directories(${EVWEB_SOURCE_DIR}/include)
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror")
SET(CMAKE_C_FLAGS_DEBUG "-DDEBUG -g3 -ggdb3")

add_library(evweb SHARED evweb.c evweb-connect-iface.c http_parser.c http-parser-callbacks.c tcp-server.c)
target_link_libraries(evweb evn ev)

INSTALL(TARGETS evweb
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)
