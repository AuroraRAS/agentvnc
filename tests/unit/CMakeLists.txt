if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR "cmake must be invoked with the top level directory")
endif()

include(GoogleTest)

# Google Test requires a new C++ standard
if(MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++17")
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++17")
endif()

enable_testing()

include_directories(${CMAKE_SOURCE_DIR}/vncviewer)

add_executable(configargs configargs.cxx)
target_link_libraries(configargs PRIVATE rfb GTest::gtest_main)
gtest_discover_tests(configargs)

add_executable(conv conv.cxx)
target_link_libraries(conv PRIVATE rfb GTest::gtest_main)
gtest_discover_tests(conv)

add_executable(convertlf convertlf.cxx)
target_link_libraries(convertlf PRIVATE core GTest::gtest_main)
gtest_discover_tests(convertlf)

add_executable(gesturehandler gesturehandler.cxx ../../vncviewer/GestureHandler.cxx)
target_link_libraries(gesturehandler PRIVATE core GTest::gtest_main)
gtest_discover_tests(gesturehandler)

add_executable(hostport hostport.cxx)
target_link_libraries(hostport PRIVATE network GTest::gtest_main)
gtest_discover_tests(hostport)

add_executable(parameters parameters.cxx)
target_link_libraries(parameters PRIVATE core GTest::gtest_main)
gtest_discover_tests(parameters)

add_executable(pixelformat pixelformat.cxx)
target_link_libraries(pixelformat PRIVATE rfb GTest::gtest_main)
gtest_discover_tests(pixelformat)

add_executable(shortcuthandler shortcuthandler.cxx ../../vncviewer/ShortcutHandler.cxx)
target_link_libraries(shortcuthandler PRIVATE core GTest::gtest_main)
gtest_discover_tests(shortcuthandler)

add_executable(unicode unicode.cxx)
target_link_libraries(unicode PRIVATE core GTest::gtest_main)
gtest_discover_tests(unicode)

add_executable(emulatemb emulatemb.cxx ../../vncviewer/EmulateMB.cxx)
target_link_libraries(emulatemb PRIVATE core GTest::gtest_main)
gtest_discover_tests(emulatemb)

add_executable(filetransfer filetransfer.cxx)
target_link_libraries(filetransfer PRIVATE rfbclient ${ZLIB_LIBRARIES} GTest::gtest_main)
gtest_discover_tests(filetransfer)

add_executable(ftpprotocol ftpprotocol.cxx ../../vncviewer/FtpProtocol.cxx)
target_link_libraries(ftpprotocol PRIVATE core GTest::gtest_main)
gtest_discover_tests(ftpprotocol)

if(NOT WIN32)
  add_executable(ftpbridge ftpbridge.cxx ../../vncviewer/FtpProtocol.cxx ../../vncviewer/FtpServer.cxx)
  target_link_libraries(ftpbridge PRIVATE rfbclient network rdr core ${ZLIB_LIBRARIES} GTest::gtest_main)
  gtest_discover_tests(ftpbridge)
endif()

if(ENABLE_H264)
  add_executable(h264decoder h264decoder.cxx)
  target_link_libraries(h264decoder PRIVATE rfbclient GTest::gtest_main)
  gtest_discover_tests(h264decoder)
endif()

if(UNIX AND NOT APPLE)
  add_executable(scrcpyvncserver_test scrcpyvncserver.cxx)
  target_link_libraries(scrcpyvncserver_test PRIVATE scrcpyvncserver_core GTest::gtest_main)
  if(ENABLE_H264)
    target_link_libraries(scrcpyvncserver_test PRIVATE rfbclient)
  endif()
  gtest_discover_tests(scrcpyvncserver_test)
endif()
