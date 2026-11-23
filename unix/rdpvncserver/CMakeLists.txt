find_package(PkgConfig REQUIRED)
pkg_check_modules(FREERDP3 REQUIRED IMPORTED_TARGET freerdp3 freerdp-client3 winpr3)

add_library(rdpvncserver_lib STATIC
  RdpPixelBuffer.cxx
  RdpH264Passthrough.cxx
  KeyMap.cxx
  RdpSession.cxx
  RDPDesktop.cxx
)

target_include_directories(rdpvncserver_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/common
)

target_include_directories(rdpvncserver_lib SYSTEM PUBLIC
  ${FREERDP3_INCLUDE_DIRS}
)

target_link_libraries(rdpvncserver_lib PUBLIC
  rfbserver
  rfb
  network
  rdr
  core
  PkgConfig::FREERDP3
)

add_executable(rdpvncserver
  rdpvncserver.cxx
)

target_link_libraries(rdpvncserver PRIVATE
  rdpvncserver_lib
)

if(SYSTEMD_FOUND)
  target_include_directories(rdpvncserver SYSTEM PRIVATE ${SYSTEMD_INCLUDE_DIRS})
  target_link_libraries(rdpvncserver PRIVATE ${SYSTEMD_LIBRARIES})
endif()

install(TARGETS rdpvncserver DESTINATION ${CMAKE_INSTALL_FULL_BINDIR})
install(FILES rdpvncserver.man DESTINATION ${CMAKE_INSTALL_FULL_MANDIR}/man1 RENAME rdpvncserver.1)
