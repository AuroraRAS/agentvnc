if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR "cmake must be invoked with the top level directory")
endif()

add_library(scrcpyvncserver_core STATIC
  AdbClient.cxx
  H264SpsParser.cxx
  ScrcpyStream.cxx
  ScrcpyControl.cxx
  ZeroCopyOutStream.cxx
  ScrcpyClient.cxx
  ScrcpyServer.cxx
  parameters.cxx
)

target_include_directories(scrcpyvncserver_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(scrcpyvncserver_core PUBLIC rfbserver rfb network rdr core)

add_executable(scrcpyvncserver
  scrcpyvncserver.cxx
)

target_link_libraries(scrcpyvncserver PRIVATE scrcpyvncserver_core)

if(SYSTEMD_FOUND)
  target_include_directories(scrcpyvncserver SYSTEM PRIVATE ${SYSTEMD_INCLUDE_DIRS})
  target_link_libraries(scrcpyvncserver PRIVATE ${SYSTEMD_LIBRARIES})
endif()

install(TARGETS scrcpyvncserver DESTINATION ${CMAKE_INSTALL_FULL_BINDIR})
install(FILES scrcpyvncserver.man DESTINATION ${CMAKE_INSTALL_FULL_MANDIR}/man1 RENAME scrcpyvncserver.1)
