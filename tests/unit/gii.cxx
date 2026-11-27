/* Copyright 2026 TigerVNC Team
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <network/TcpSocket.h>

#include <rdr/FdInStream.h>
#include <rdr/FdOutStream.h>
#include <rdr/InStream.h>
#include <rdr/MemInStream.h>
#include <rdr/MemOutStream.h>

#include <rfb/AccessRights.h>
#include <rfb/CConnection.h>
#include <rfb/CMsgHandler.h>
#include <rfb/CMsgReader.h>
#include <rfb/CMsgWriter.h>
#include <rfb/Exception.h>
#include <rfb/PixelBuffer.h>
#include <rfb/PixelFormat.h>
#include <rfb/SConnection.h>
#include <rfb/SDesktop.h>
#include <rfb/SMsgHandler.h>
#include <rfb/SMsgReader.h>
#include <rfb/SMsgWriter.h>
#include <rfb/SecurityClient.h>
#include <rfb/SecurityServer.h>
#include <rfb/ServerCore.h>
#include <rfb/VNCServerST.h>
#include <rfb/encodings.h>
#include <rfb/giiTypes.h>
#include <rfb/msgTypes.h>

#include "GIIDeviceManager.h"

namespace {

// A stream that releases at most `chunkSize` bytes per feed() step, exercising
// incremental reads and RFB_INSTREAM_CHECK restore-point handling.
class ChunkedInStream : public rdr::InStream {
public:
  ChunkedInStream(const uint8_t* data_, size_t len_, size_t chunkSize_ = 1)
    : data(data_), len(len_), offset(0), availLimit(0), chunkSize(chunkSize_),
      buf(len_ > 0 ? len_ : 1)
  {
    ptr = buf.data();
    end = buf.data();
  }

  size_t pos() override { return offset - (size_t)(end - ptr); }

  bool feed()
  {
    if (availLimit >= len)
      return false;
    availLimit = std::min(len, availLimit + chunkSize);
    return true;
  }

private:
  bool overrun(size_t needed) override
  {
    size_t buffered = (size_t)(end - ptr);
    if (buffered >= needed)
      return true;

    size_t canAdd = availLimit - offset;
    if (buffered + canAdd < needed)
      return false;

    if (buffered > 0 && ptr != buf.data())
      memmove(buf.data(), ptr, buffered);

    if (canAdd > 0) {
      memcpy(buf.data() + buffered, data + offset, canAdd);
      offset += canAdd;
    }

    ptr = buf.data();
    end = buf.data() + buffered + canAdd;
    return true;
  }

  const uint8_t* data;
  size_t len;
  size_t offset;
  size_t availLimit;
  size_t chunkSize;
  std::vector<uint8_t> buf;
};

template <typename Reader>
static void readAllChunked(Reader& reader, ChunkedInStream& cis, size_t totalLen)
{
  while (cis.pos() < totalLen) {
    while (reader.readMsg()) {}
    if (!cis.feed())
      break;
  }
  while (reader.readMsg()) {}
}

static void writeEU16(rdr::OutStream* os, uint16_t v, bool bigEndian)
{
  if (bigEndian) {
    os->writeU8((uint8_t)(v >> 8));
    os->writeU8((uint8_t)(v & 0xff));
  } else {
    os->writeU8((uint8_t)(v & 0xff));
    os->writeU8((uint8_t)(v >> 8));
  }
}

static void writeEU32(rdr::OutStream* os, uint32_t v, bool bigEndian)
{
  if (bigEndian) {
    os->writeU8((uint8_t)((v >> 24) & 0xff));
    os->writeU8((uint8_t)((v >> 16) & 0xff));
    os->writeU8((uint8_t)((v >> 8) & 0xff));
    os->writeU8((uint8_t)(v & 0xff));
  } else {
    os->writeU8((uint8_t)(v & 0xff));
    os->writeU8((uint8_t)((v >> 8) & 0xff));
    os->writeU8((uint8_t)((v >> 16) & 0xff));
    os->writeU8((uint8_t)((v >> 24) & 0xff));
  }
}

static void writeES32(rdr::OutStream* os, int32_t v, bool bigEndian)
{
  writeEU32(os, (uint32_t)v, bigEndian);
}

static void writePaddedStr(rdr::OutStream* os, const std::string& s, size_t n)
{
  size_t copyLen = std::min(s.size(), n - 1);
  if (copyLen > 0)
    os->writeBytes((const uint8_t*)s.data(), copyLen);
  os->pad(n - copyLen);
}

class MockCMsgHandler : public rfb::CMsgHandler {
public:
  struct VersionCall { uint16_t maxV, minV; };
  std::vector<VersionCall> versions;
  std::vector<uint32_t> createdOrigins;
  int bells = 0;

  void setDesktopSize(int, int) override {}
  void setExtendedDesktopSize(unsigned, unsigned, int, int,
                              const rfb::ScreenSet&) override {}
  void setCursor(int, int, const core::Point&, const uint8_t*) override {}
  void setCursorPos(const core::Point&) override {}
  void setName(const char*) override {}
  void fence(uint32_t, unsigned, const uint8_t[]) override {}
  void endOfContinuousUpdates() override {}
  void supportsQEMUKeyEvent() override {}
  void supportsQEMUAudio() override {}
  void supportsExtendedMouseButtons() override {}
  void serverInit(int, int, const rfb::PixelFormat&, const char*) override {}
  bool readAndDecodeRect(const core::Rect&, int, const rfb::PixelFormat&,
                         rfb::ModifiablePixelBuffer*) override { return true; }
  void framebufferUpdateStart() override {}
  void framebufferUpdateEnd() override {}
  bool dataRect(const core::Rect&, int) override { return true; }
  void setColourMapEntries(int, int, uint16_t*) override {}
  void bell() override { bells++; }
  void serverCutText(const char*) override {}
  void setLEDState(unsigned int) override {}
  void clipboardCaps(uint32_t, const uint32_t*) override {}
  void clipboardRequest(uint32_t) override {}
  void clipboardPeek() override {}
  void clipboardNotify(uint32_t) override {}
  void clipboardProvide(uint32_t, const size_t*,
                        const uint8_t* const*) override {}
  void audioBegin() override {}
  void audioEnd() override {}
  void audioData(const uint8_t*, size_t) override {}

  void giiServerVersion(uint16_t maxVersion, uint16_t minVersion) override
  {
    versions.push_back({maxVersion, minVersion});
  }
  void giiDeviceCreated(uint32_t deviceOrigin) override
  {
    createdOrigins.push_back(deviceOrigin);
  }
};

class MockSMsgHandler : public rfb::SMsgHandler {
public:
  std::vector<uint16_t> versions;
  std::vector<rfb::GIIDevice> createdDevices;
  std::vector<uint32_t> destroyedOrigins;
  std::vector<rfb::GIIEvent> events;
  std::vector<bool> updateRequests;

  void clientInit(bool) override {}
  void setPixelFormat(const rfb::PixelFormat&) override {}
  void setEncodings(int, const int32_t*) override {}
  void framebufferUpdateRequest(const core::Rect&, bool inc) override
  {
    updateRequests.push_back(inc);
  }
  void setDesktopSize(int, int, const rfb::ScreenSet&) override {}
  void fence(uint32_t, unsigned, const uint8_t[]) override {}
  void enableContinuousUpdates(bool, int, int, int, int) override {}
  void keyEvent(uint32_t, uint32_t, bool) override {}
  void pointerEvent(const core::Point&, uint16_t) override {}
  void clientCutText(const char*) override {}
  void clipboardCaps(uint32_t, const uint32_t*) override {}
  void clipboardRequest(uint32_t) override {}
  void clipboardPeek() override {}
  void clipboardNotify(uint32_t) override {}
  void clipboardProvide(uint32_t, const size_t*,
                        const uint8_t* const*) override {}

  void giiVersion(uint16_t version) override { versions.push_back(version); }
  void giiDeviceCreate(const rfb::GIIDevice& device) override
  {
    createdDevices.push_back(device);
  }
  void giiDeviceDestroy(uint32_t deviceOrigin) override
  {
    destroyedOrigins.push_back(deviceOrigin);
  }
  void giiEvent(const rfb::GIIEvent& event) override
  {
    events.push_back(event);
  }
};

class MockSDesktop : public rfb::SDesktop {
public:
  MockSDesktop(bool giiSupported_ = true)
    : giiSupported(giiSupported_), vs(nullptr), pb(nullptr), nextOrigin(100)
  {}

  ~MockSDesktop() { delete pb; }

  void init(rfb::VNCServer* vs_) override { vs = vs_; }
  void start() override
  {
    if (!pb) {
      rfb::PixelFormat pf(32, 24, false, true, 255, 255, 255, 0, 8, 16);
      pb = new rfb::ManagedPixelBuffer(pf, 800, 600);
      vs->setPixelBuffer(pb);
    }
  }
  void queryConnection(rfb::SConnection* conn) override
  {
    vs->approveConnection(conn, true);
  }
  void terminate() override {}

  bool supportsGII() const override { return giiSupported; }

  uint32_t giiDeviceCreate(const rfb::GIIDevice& device) override
  {
    uint32_t origin = nextOrigin++;
    activeDevices[origin] = device;
    createdDevices.push_back(device);
    return origin;
  }

  void giiDeviceDestroy(uint32_t deviceOrigin) override
  {
    destroyedOrigins.push_back(deviceOrigin);
    activeDevices.erase(deviceOrigin);
  }

  void giiEvent(const rfb::GIIEvent& event) override
  {
    events.push_back(event);
  }

  bool giiSupported;
  rfb::VNCServer* vs;
  rfb::ManagedPixelBuffer* pb;
  uint32_t nextOrigin;
  std::map<uint32_t, rfb::GIIDevice> activeDevices;
  std::vector<rfb::GIIDevice> createdDevices;
  std::vector<uint32_t> destroyedOrigins;
  std::vector<rfb::GIIEvent> events;
};

class TestCConnection : public rfb::CConnection {
public:
  TestCConnection(bool enableGII_)
    : initDoneCalled(false)
  {
    supportsGII = enableGII_;
  }

  void getUserPasswd(std::string*, std::string*) override {}
  void bell() override {}
  bool verifyCertificate(unsigned int, const uint8_t*, size_t) override
  {
    return true;
  }
  bool verifyHostKey(const uint8_t*, size_t, const char*) override
  {
    return true;
  }

  void initDone() override
  {
    initDoneCalled = true;
    resizeFramebuffer();
  }

  void resizeFramebuffer() override
  {
    setFramebuffer(new rfb::ManagedPixelBuffer(server.pf(),
                                               server.width(),
                                               server.height()));
  }

  void handleGIIEnabled(uint16_t version) override
  {
    enabledVersions.push_back(version);
  }

  void handleGIIDeviceCreated(uint32_t deviceOrigin) override
  {
    createdOrigins.push_back(deviceOrigin);
  }

  bool initDoneCalled;
  std::vector<uint16_t> enabledVersions;
  std::vector<uint32_t> createdOrigins;
};

struct SocketPair {
  SocketPair()
  {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    network::TcpListener listener(reinterpret_cast<const sockaddr*>(&address),
                                  sizeof(address));
    client.reset(new network::TcpSocket("127.0.0.1", listener.getMyPort()));
    server.reset(listener.accept());
  }

  std::unique_ptr<network::Socket> client;
  std::unique_ptr<network::Socket> server;
};

static void pumpPair(TestCConnection* client, network::Socket* clientSock,
                     rfb::VNCServerST* server, network::Socket* serverSock,
                     int maxIters = 200)
{
  for (int i = 0; i < maxIters; i++) {
    bool didWork = false;
    clientSock->outStream().flush();
    serverSock->outStream().flush();

    while (client->state() != rfb::CConnection::RFBSTATE_CLOSING &&
           client->state() != rfb::CConnection::RFBSTATE_INVALID &&
           client->processMsg()) {
      didWork = true;
    }

    rfb::SConnection* sconn = server->getConnection(serverSock);
    if (sconn != nullptr &&
        sconn->state() != rfb::SConnection::RFBSTATE_CLOSING &&
        sconn->state() != rfb::SConnection::RFBSTATE_INVALID) {
      size_t before = serverSock->inStream().pos();
      server->processSocketReadEvent(serverSock);
      if (serverSock->inStream().pos() != before)
        didWork = true;
    }

    if (!didWork)
      break;
  }
}

class SecurityParamGuard {
public:
  SecurityParamGuard()
    : savedClient(rfb::SecurityClient::secTypes.getValueStr()),
      savedServer(rfb::SecurityServer::secTypes.getValueStr()),
      savedAcceptKey(rfb::Server::acceptKeyEvents),
      savedAcceptPtr(rfb::Server::acceptPointerEvents)
  {
    rfb::SecurityClient::secTypes.setParam("None");
    rfb::SecurityServer::secTypes.setParam("None");
    rfb::Server::acceptKeyEvents.setParam(true);
    rfb::Server::acceptPointerEvents.setParam(true);
  }
  ~SecurityParamGuard()
  {
    rfb::SecurityClient::secTypes.setParam(savedClient.c_str());
    rfb::SecurityServer::secTypes.setParam(savedServer.c_str());
    rfb::Server::acceptKeyEvents.setParam(savedAcceptKey);
    rfb::Server::acceptPointerEvents.setParam(savedAcceptPtr);
  }

private:
  std::string savedClient;
  std::string savedServer;
  bool savedAcceptKey;
  bool savedAcceptPtr;
};

class MockGIIEventSender : public GIIEventSender {
public:
  void createGIIDevice(const rfb::GIIDevice& device) override
  {
    createdRequests.push_back(device);
  }
  void destroyGIIDevice(uint32_t deviceOrigin) override
  {
    destroyedOrigins.push_back(deviceOrigin);
  }
  void sendGIIEvents(const std::vector<rfb::GIIEvent>& events) override
  {
    sentBatches.push_back(events);
    for (const rfb::GIIEvent& ev : events)
      sentEvents.push_back(ev);
  }

  std::vector<rfb::GIIDevice> createdRequests;
  std::vector<uint32_t> destroyedOrigins;
  std::vector<std::vector<rfb::GIIEvent>> sentBatches;
  std::vector<rfb::GIIEvent> sentEvents;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. Wire Serialization & Parsing Round-Trips (Big-Endian & Little-Endian)
// ---------------------------------------------------------------------------

TEST(GIIProtocol, ServerMessagesBigAndLittleEndian)
{
  for (int be = 0; be <= 1; be++) {
    bool bigEndian = (be != 0);
    rdr::MemOutStream mos;

    // giiServerVersion
    mos.writeU8(rfb::msgTypeGIIServerMessage);
    mos.writeU8((bigEndian ? rfb::giiBigEndian : 0) | rfb::giiServerVersion);
    writeEU16(&mos, 4, bigEndian);
    writeEU16(&mos, 3, bigEndian);
    writeEU16(&mos, 1, bigEndian);

    // giiServerDeviceCreateResponse (valid)
    mos.writeU8(rfb::msgTypeGIIServerMessage);
    mos.writeU8((bigEndian ? rfb::giiBigEndian : 0) |
                rfb::giiServerDeviceCreateResponse);
    writeEU16(&mos, 4, bigEndian);
    writeEU32(&mos, 0x12345678U, bigEndian);

    // giiServerDeviceCreateResponse (failure = 0)
    mos.writeU8(rfb::msgTypeGIIServerMessage);
    mos.writeU8((bigEndian ? rfb::giiBigEndian : 0) |
                rfb::giiServerDeviceCreateResponse);
    writeEU16(&mos, 4, bigEndian);
    writeEU32(&mos, 0, bigEndian);

    // Incremental 1-byte reads
    ChunkedInStream cis(mos.data(), mos.length(), 1);
    MockCMsgHandler handler;
    rfb::CMsgReader reader(&handler, &cis);

    readAllChunked(reader, cis, mos.length());
    EXPECT_EQ(cis.pos(), mos.length());

    ASSERT_EQ(handler.versions.size(), 1U);
    EXPECT_EQ(handler.versions[0].maxV, 3);
    EXPECT_EQ(handler.versions[0].minV, 1);

    ASSERT_EQ(handler.createdOrigins.size(), 2U);
    EXPECT_EQ(handler.createdOrigins[0], 0x12345678U);
    EXPECT_EQ(handler.createdOrigins[1], 0U);
  }
}

TEST(GIIProtocol, ClientMessagesWriterRoundTrip)
{
  rfb::ServerParams sp;
  sp.supportsGII = true;

  rdr::MemOutStream mos;
  rfb::CMsgWriter writer(&sp, &mos);

  writer.writeGIIVersion(1);

  // Device with 0 valuators
  rfb::GIIDevice padDev;
  padDev.name = "Wacom Pad";
  padDev.vendorID = 0x056a;
  padDev.productID = 0x0357;
  padDev.canGenerate = rfb::giiEventMaskPtrButtonPress |
                       rfb::giiEventMaskPtrButtonRelease;
  padDev.numRegisters = 2;
  padDev.numButtons = 8;
  writer.writeGIIDeviceCreate(padDev);

  // Device with >31-char name, >74-char longName, >4-char shortName, negative ranges
  rfb::GIIDevice stylusDev;
  stylusDev.name = "Extremely Long Tablet Stylus Name Exceeding 31 Characters";
  stylusDev.vendorID = 0x056a;
  stylusDev.productID = 0x0358;
  stylusDev.canGenerate = rfb::giiEventMaskValuatorAbsolute |
                          rfb::giiEventMaskPtrButtonPress |
                          rfb::giiEventMaskPtrButtonRelease;
  stylusDev.numButtons = 3;

  rfb::GIIValuator v1;
  v1.index = 0;
  v1.longName = std::string(90, 'L');
  v1.shortName = "LongShortName";
  v1.rangeMin = -1000;
  v1.rangeCenter = 0;
  v1.rangeMax = 1000;
  v1.siUnit = rfb::giiUnitLength;
  v1.siAdd = -500;
  v1.siMul = 1;
  v1.siDiv = 2000;
  v1.siShift = -2;
  stylusDev.valuators.push_back(v1);

  rfb::GIIValuator v2;
  v2.index = 1;
  v2.longName = "Abs Pressure";
  v2.shortName = "Prs";
  v2.rangeMin = 0;
  v2.rangeCenter = 4096;
  v2.rangeMax = 8191;
  v2.siUnit = rfb::giiUnitForce;
  v2.siDiv = 1;
  stylusDev.valuators.push_back(v2);

  writer.writeGIIDeviceCreate(stylusDev);
  writer.writeGIIDeviceDestroy(42);

  // All 9 event types in a single batch
  std::vector<rfb::GIIEvent> batch;
  uint8_t keyTypes[3] = {
    rfb::giiEventKeyPress, rfb::giiEventKeyRelease, rfb::giiEventKeyRepeat
  };
  for (int i = 0; i < 3; i++) {
    rfb::GIIEvent ev;
    ev.type = keyTypes[i];
    ev.origin = 10 + (uint32_t)i;
    ev.modifiers = 0x11;
    ev.symbol = 0x0061 + (uint32_t)i;
    ev.label = 0x0041 + (uint32_t)i;
    ev.button = 30 + (uint32_t)i;
    batch.push_back(ev);
  }

  for (int i = 0; i < 2; i++) {
    rfb::GIIEvent ev;
    ev.type = (i == 0) ? rfb::giiEventPtrRelative : rfb::giiEventPtrAbsolute;
    ev.origin = 20 + (uint32_t)i;
    ev.x = -123;
    ev.y = 456;
    ev.z = -7;
    ev.wheel = 2;
    batch.push_back(ev);
  }

  for (int i = 0; i < 2; i++) {
    rfb::GIIEvent ev;
    ev.type = (i == 0) ? rfb::giiEventPtrButtonPress
                       : rfb::giiEventPtrButtonRelease;
    ev.origin = 30 + (uint32_t)i;
    ev.button = 1 + (uint32_t)i;
    batch.push_back(ev);
  }

  for (int i = 0; i < 2; i++) {
    rfb::GIIEvent ev;
    ev.type = (i == 0) ? rfb::giiEventValuatorRelative
                       : rfb::giiEventValuatorAbsolute;
    ev.origin = 40 + (uint32_t)i;
    ev.first = 1;
    ev.values.push_back(-500);
    ev.values.push_back(0);
    ev.values.push_back(8191);
    batch.push_back(ev);
  }

  writer.writeGIIEvents(batch);

  // Read incrementally 1 byte at a time
  ChunkedInStream cis(mos.data(), mos.length(), 1);
  MockSMsgHandler handler;
  rfb::SMsgReader reader(&handler, &cis);
  readAllChunked(reader, cis, mos.length());
  EXPECT_EQ(cis.pos(), mos.length());

  ASSERT_EQ(handler.versions.size(), 1U);
  EXPECT_EQ(handler.versions[0], 1);

  ASSERT_EQ(handler.createdDevices.size(), 2U);
  EXPECT_EQ(handler.createdDevices[0].name, "Wacom Pad");
  EXPECT_EQ(handler.createdDevices[0].valuators.size(), 0U);
  EXPECT_EQ(handler.createdDevices[0].numButtons, 8U);

  const rfb::GIIDevice& parsedStylus = handler.createdDevices[1];
  EXPECT_EQ(parsedStylus.name.size(), rfb::giiDeviceNameLen - 1);
  EXPECT_EQ(parsedStylus.name, stylusDev.name.substr(0, rfb::giiDeviceNameLen - 1));
  ASSERT_EQ(parsedStylus.valuators.size(), 2U);
  EXPECT_EQ(parsedStylus.valuators[0].longName,
            std::string(rfb::giiValuatorLongNameLen - 1, 'L'));
  EXPECT_EQ(parsedStylus.valuators[0].shortName, "Long");
  EXPECT_EQ(parsedStylus.valuators[0].rangeMin, -1000);
  EXPECT_EQ(parsedStylus.valuators[0].rangeCenter, 0);
  EXPECT_EQ(parsedStylus.valuators[0].rangeMax, 1000);
  EXPECT_EQ(parsedStylus.valuators[0].siAdd, -500);
  EXPECT_EQ(parsedStylus.valuators[0].siDiv, 2000);
  EXPECT_EQ(parsedStylus.valuators[0].siShift, -2);
  EXPECT_EQ(parsedStylus.valuators[1].shortName, "Prs");

  ASSERT_EQ(handler.destroyedOrigins.size(), 1U);
  EXPECT_EQ(handler.destroyedOrigins[0], 42U);

  ASSERT_EQ(handler.events.size(), 9U);
  EXPECT_EQ(handler.events[0].type, rfb::giiEventKeyPress);
  EXPECT_EQ(handler.events[0].symbol, 0x0061U);
  EXPECT_EQ(handler.events[3].type, rfb::giiEventPtrRelative);
  EXPECT_EQ(handler.events[3].x, -123);
  EXPECT_EQ(handler.events[3].y, 456);
  EXPECT_EQ(handler.events[3].z, -7);
  EXPECT_EQ(handler.events[3].wheel, 2);
  EXPECT_EQ(handler.events[5].type, rfb::giiEventPtrButtonPress);
  EXPECT_EQ(handler.events[5].button, 1U);
  EXPECT_EQ(handler.events[7].type, rfb::giiEventValuatorRelative);
  ASSERT_EQ(handler.events[7].values.size(), 3U);
  EXPECT_EQ(handler.events[7].values[0], -500);
  EXPECT_EQ(handler.events[8].values[2], 8191);
}

TEST(GIIProtocol, ClientMessagesLittleEndian)
{
  rdr::MemOutStream mos;

  // Little-endian Version
  mos.writeU8(rfb::msgTypeGIIClientMessage);
  mos.writeU8(rfb::giiClientVersion);
  writeEU16(&mos, 2, false);
  writeEU16(&mos, 1, false);

  // Little-endian DeviceCreate with 1 valuator
  mos.writeU8(rfb::msgTypeGIIClientMessage);
  mos.writeU8(rfb::giiClientDeviceCreate);
  writeEU16(&mos, (uint16_t)(rfb::giiDeviceCreateFixedSize +
                             rfb::giiValuatorWireSize), false);
  writePaddedStr(&mos, "LE Tablet", rfb::giiDeviceNameLen);
  writeEU32(&mos, 0x1111, false);
  writeEU32(&mos, 0x2222, false);
  writeEU32(&mos, rfb::giiEventMaskValuatorAbsolute, false);
  writeEU32(&mos, 0, false);
  writeEU32(&mos, 1, false);
  writeEU32(&mos, 2, false);
  writeEU32(&mos, 0, false);
  writePaddedStr(&mos, "Abs X", rfb::giiValuatorLongNameLen);
  writePaddedStr(&mos, "X", rfb::giiValuatorShortNameLen);
  writeES32(&mos, -10, false);
  writeES32(&mos, 5, false);
  writeES32(&mos, 20, false);
  writeEU32(&mos, rfb::giiUnitLength, false);
  writeES32(&mos, 0, false);
  writeES32(&mos, 1, false);
  writeES32(&mos, 1, false);
  writeES32(&mos, 0, false);

  // Little-endian Event
  mos.writeU8(rfb::msgTypeGIIClientMessage);
  mos.writeU8(rfb::giiClientEvent);
  writeEU16(&mos, 20, false);
  mos.writeU8(20);
  mos.writeU8(rfb::giiEventValuatorAbsolute);
  mos.pad(2);
  writeEU32(&mos, 99, false);
  writeEU32(&mos, 0, false);
  writeEU32(&mos, 1, false);
  writeES32(&mos, -7, false);

  // Little-endian DeviceDestroy
  mos.writeU8(rfb::msgTypeGIIClientMessage);
  mos.writeU8(rfb::giiClientDeviceDestroy);
  writeEU16(&mos, 4, false);
  writeEU32(&mos, 99, false);

  ChunkedInStream cis(mos.data(), mos.length(), 1);
  MockSMsgHandler handler;
  rfb::SMsgReader reader(&handler, &cis);
  readAllChunked(reader, cis, mos.length());

  ASSERT_EQ(handler.versions.size(), 1U);
  EXPECT_EQ(handler.versions[0], 1);
  ASSERT_EQ(handler.createdDevices.size(), 1U);
  EXPECT_EQ(handler.createdDevices[0].name, "LE Tablet");
  EXPECT_EQ(handler.createdDevices[0].vendorID, 0x1111U);
  EXPECT_EQ(handler.createdDevices[0].valuators[0].rangeMin, -10);
  ASSERT_EQ(handler.events.size(), 1U);
  EXPECT_EQ(handler.events[0].origin, 99U);
  EXPECT_EQ(handler.events[0].values[0], -7);
  ASSERT_EQ(handler.destroyedOrigins.size(), 1U);
  EXPECT_EQ(handler.destroyedOrigins[0], 99U);
}

TEST(GIIProtocol, UnknownSubtypesEventsAndErrors)
{
  // Unknown server subtype followed by Bell -> stream stays synchronized
  {
    rdr::MemOutStream mos;
    mos.writeU8(rfb::msgTypeGIIServerMessage);
    mos.writeU8(rfb::giiBigEndian | 99);
    mos.writeU16(5);
    mos.writeBytes((const uint8_t*)"hello", 5);
    mos.writeU8(rfb::msgTypeBell);

    rdr::MemInStream mis(mos.data(), mos.length());
    MockCMsgHandler handler;
    rfb::CMsgReader reader(&handler, &mis);
    EXPECT_TRUE(reader.readMsg());
    EXPECT_TRUE(reader.readMsg());
    EXPECT_EQ(handler.bells, 1);
  }

  // Unknown client subtype + unknown event type followed by valid event and FBU req
  {
    rdr::MemOutStream mos;
    mos.writeU8(rfb::msgTypeGIIClientMessage);
    mos.writeU8(rfb::giiBigEndian | 77);
    mos.writeU16(3);
    mos.pad(3);

    // Event batch with unknown event (size 10, type 99) + valid button press (size 12)
    mos.writeU8(rfb::msgTypeGIIClientMessage);
    mos.writeU8(rfb::giiBigEndian | rfb::giiClientEvent);
    mos.writeU16(22);
    mos.writeU8(10);
    mos.writeU8(99);
    mos.pad(8);
    mos.writeU8(12);
    mos.writeU8(rfb::giiEventPtrButtonPress);
    mos.pad(2);
    mos.writeU32(5);
    mos.writeU32(1);

    mos.writeU8(rfb::msgTypeFramebufferUpdateRequest);
    mos.writeU8(1);
    mos.writeU16(0);
    mos.writeU16(0);
    mos.writeU16(10);
    mos.writeU16(10);

    rdr::MemInStream mis(mos.data(), mos.length());
    MockSMsgHandler handler;
    rfb::SMsgReader reader(&handler, &mis);
    EXPECT_TRUE(reader.readMsg());
    EXPECT_TRUE(reader.readMsg());
    EXPECT_TRUE(reader.readMsg());
    ASSERT_EQ(handler.events.size(), 1U);
    EXPECT_EQ(handler.events[0].type, rfb::giiEventPtrButtonPress);
    EXPECT_EQ(handler.events[0].origin, 5U);
    EXPECT_EQ(handler.updateRequests.size(), 1U);
  }

  // Malformed server version length
  {
    rdr::MemOutStream mos;
    mos.writeU8(rfb::msgTypeGIIServerMessage);
    mos.writeU8(rfb::giiBigEndian | rfb::giiServerVersion);
    mos.writeU16(2);
    mos.writeU16(1);

    rdr::MemInStream mis(mos.data(), mos.length());
    MockCMsgHandler handler;
    rfb::CMsgReader reader(&handler, &mis);
    EXPECT_THROW(reader.readMsg(), rfb::protocol_error);
  }

  // Malformed client event size & valuator count
  {
    rdr::MemOutStream mos;
    mos.writeU8(rfb::msgTypeGIIClientMessage);
    mos.writeU8(rfb::giiBigEndian | rfb::giiClientEvent);
    mos.writeU16(16);
    mos.writeU8(16);
    mos.writeU8(rfb::giiEventValuatorAbsolute);
    mos.pad(2);
    mos.writeU32(1);
    mos.writeU32(0);
    mos.writeU32(rfb::giiMaxValuatorsPerEvent + 1);

    rdr::MemInStream mis(mos.data(), mos.length());
    MockSMsgHandler handler;
    rfb::SMsgReader reader(&handler, &mis);
    EXPECT_THROW(reader.readMsg(), rfb::protocol_error);
  }

  // Writer guards when GII is unsupported
  {
    rfb::ServerParams sp;
    rdr::MemOutStream mos;
    rfb::CMsgWriter writer(&sp, &mos);
    EXPECT_THROW(writer.writeGIIVersion(1), std::logic_error);
  }
}

// ---------------------------------------------------------------------------
// 2. End-to-End CConnection <-> VNCServerST / VNCSConnectionST / SDesktop
// ---------------------------------------------------------------------------

TEST(GIIEndToEnd, DisabledDesktopDoesNotEnableGII)
{
  SecurityParamGuard guard;
  SocketPair sockets;
  auto* clientSock = sockets.client.get();
  auto* serverSock = sockets.server.get();

  MockSDesktop desktop(false);
  rfb::VNCServerST server("test-no-gii", &desktop);
  ASSERT_TRUE(server.addSocket(serverSock));

  TestCConnection client(true);
  client.setStreams(&clientSock->inStream(), &clientSock->outStream());
  client.initialiseProtocol();

  pumpPair(&client, clientSock, &server, serverSock);

  EXPECT_EQ(client.state(), rfb::CConnection::RFBSTATE_NORMAL);
  EXPECT_TRUE(client.initDoneCalled);
  EXPECT_FALSE(client.server.supportsGII);
  EXPECT_TRUE(client.enabledVersions.empty());

  server.removeSocket(serverSock);
}

TEST(GIIEndToEnd, HandshakeDeviceLifecycleEventsAndCleanup)
{
  SecurityParamGuard guard;
  SocketPair sockets;
  auto* clientSock = sockets.client.get();
  auto* serverSock = sockets.server.get();

  MockSDesktop desktop(true);
  rfb::VNCServerST server("test-gii", &desktop);
  ASSERT_TRUE(server.addSocket(serverSock));

  TestCConnection client(true);
  client.setStreams(&clientSock->inStream(), &clientSock->outStream());
  client.initialiseProtocol();

  pumpPair(&client, clientSock, &server, serverSock);

  ASSERT_EQ(client.state(), rfb::CConnection::RFBSTATE_NORMAL);
  ASSERT_TRUE(client.server.supportsGII);
  EXPECT_EQ(client.server.giiVersion, 1);
  ASSERT_EQ(client.enabledVersions.size(), 1U);
  EXPECT_EQ(client.enabledVersions[0], 1);

  // Create two devices
  rfb::GIIDevice dev1;
  dev1.name = "Stylus";
  dev1.canGenerate = rfb::giiEventMaskValuatorAbsolute |
                     rfb::giiEventMaskPtrButtonPress |
                     rfb::giiEventMaskPtrButtonRelease;
  dev1.numButtons = 2;
  rfb::GIIValuator val;
  val.index = 0;
  val.longName = "Abs X";
  val.shortName = "X";
  val.rangeMax = 1000;
  val.siDiv = 1;
  dev1.valuators.push_back(val);

  rfb::GIIDevice dev2;
  dev2.name = "Pad";
  dev2.canGenerate = rfb::giiEventMaskPtrButtonPress |
                     rfb::giiEventMaskPtrButtonRelease;
  dev2.numButtons = 4;

  client.createGIIDevice(dev1);
  client.createGIIDevice(dev2);
  pumpPair(&client, clientSock, &server, serverSock);

  ASSERT_EQ(client.createdOrigins.size(), 2U);
  uint32_t origin1 = client.createdOrigins[0];
  uint32_t origin2 = client.createdOrigins[1];
  EXPECT_NE(origin1, 0U);
  EXPECT_NE(origin2, 0U);
  EXPECT_NE(origin1, origin2);
  EXPECT_EQ(desktop.activeDevices.size(), 2U);

  // Send events for dev1
  rfb::GIIEvent ev;
  ev.type = rfb::giiEventValuatorAbsolute;
  ev.origin = origin1;
  ev.first = 0;
  ev.values.push_back(500);
  client.sendGIIEvent(ev);

  rfb::GIIEvent btn;
  btn.type = rfb::giiEventPtrButtonPress;
  btn.origin = origin1;
  btn.button = 1;
  client.sendGIIEvent(btn);

  pumpPair(&client, clientSock, &server, serverSock);

  ASSERT_EQ(desktop.events.size(), 2U);
  EXPECT_EQ(desktop.events[0].origin, origin1);
  EXPECT_EQ(desktop.events[0].values[0], 500);
  EXPECT_EQ(desktop.events[1].type, rfb::giiEventPtrButtonPress);

  // Events must refer to a created device and match its capabilities.
  btn.origin = 0;
  client.sendGIIEvent(btn);
  btn.origin = origin1;
  btn.button = 3;
  client.sendGIIEvent(btn);
  ev.first = 1;
  client.sendGIIEvent(ev);
  ev.first = 0;
  ev.type = rfb::giiEventValuatorRelative;
  client.sendGIIEvent(ev);
  pumpPair(&client, clientSock, &server, serverSock);
  EXPECT_EQ(desktop.events.size(), 2U);

  // Explicitly destroy dev1, leave dev2 open for disconnect cleanup
  client.destroyGIIDevice(origin1);
  pumpPair(&client, clientSock, &server, serverSock);
  EXPECT_EQ(desktop.activeDevices.size(), 1U);
  ASSERT_EQ(desktop.destroyedOrigins.size(), 1U);
  EXPECT_EQ(desktop.destroyedOrigins[0], origin1);

  // Disconnecting must destroy dev2 even if support has gone away.
  desktop.giiSupported = false;
  server.removeSocket(serverSock);
  EXPECT_EQ(desktop.activeDevices.size(), 0U);
  ASSERT_EQ(desktop.destroyedOrigins.size(), 2U);
  EXPECT_EQ(desktop.destroyedOrigins[1], origin2);
}

TEST(GIIEndToEnd, AccessRightsAndCrossClientIsolation)
{
  SecurityParamGuard guard;
  SocketPair socketsA, socketsB, socketsC;
  auto* cSockA = socketsA.client.get();
  auto* sSockA = socketsA.server.get();
  auto* cSockB = socketsB.client.get();
  auto* sSockB = socketsB.server.get();
  auto* cSockC = socketsC.client.get();
  auto* sSockC = socketsC.server.get();

  MockSDesktop desktop(true);
  rfb::VNCServerST server("test-isolation", &desktop);
  ASSERT_TRUE(server.addSocket(sSockA, false, rfb::AccessDefault));
  ASSERT_TRUE(server.addSocket(sSockB, false,
                               rfb::AccessView | rfb::AccessNoQuery));

  TestCConnection clientA(true), clientB(true);
  clientA.setShared(true);
  clientB.setShared(true);
  clientA.setStreams(&cSockA->inStream(), &cSockA->outStream());
  clientB.setStreams(&cSockB->inStream(), &cSockB->outStream());
  clientA.initialiseProtocol();
  clientB.initialiseProtocol();

  pumpPair(&clientA, cSockA, &server, sSockA);
  pumpPair(&clientB, cSockB, &server, sSockB);

  ASSERT_TRUE(clientA.server.supportsGII);
  ASSERT_TRUE(clientB.server.supportsGII);

  // Client A creates device
  rfb::GIIDevice dev;
  dev.name = "Stylus A";
  dev.canGenerate = rfb::giiEventMaskPtrButtonPress | rfb::giiEventMaskKeyPress;
  dev.numButtons = 1;
  clientA.createGIIDevice(dev);
  pumpPair(&clientA, cSockA, &server, sSockA);
  ASSERT_EQ(clientA.createdOrigins.size(), 1U);
  uint32_t originA = clientA.createdOrigins[0];
  EXPECT_NE(originA, 0U);

  // View-only Client B must be refused device creation (origin == 0)
  clientB.createGIIDevice(dev);
  pumpPair(&clientB, cSockB, &server, sSockB);
  ASSERT_EQ(clientB.createdOrigins.size(), 1U);
  EXPECT_EQ(clientB.createdOrigins[0], 0U);

  // Now test cross-client isolation with a full-rights Client C
  ASSERT_TRUE(server.addSocket(sSockC, false, rfb::AccessDefault));

  TestCConnection clientC(true);
  clientC.setShared(true);
  clientC.setStreams(&cSockC->inStream(), &cSockC->outStream());
  clientC.initialiseProtocol();
  pumpPair(&clientC, cSockC, &server, sSockC);
  ASSERT_TRUE(clientC.server.supportsGII);

  // Client C tries to inject event on and destroy Client A's origin -> ignored!
  rfb::GIIEvent spoof;
  spoof.type = rfb::giiEventPtrButtonPress;
  spoof.origin = originA;
  spoof.button = 1;
  clientC.sendGIIEvent(spoof);
  clientC.destroyGIIDevice(originA);
  pumpPair(&clientC, cSockC, &server, sSockC);

  EXPECT_TRUE(desktop.events.empty());
  EXPECT_EQ(desktop.activeDevices.count(originA), 1U);

  // Server::acceptPointerEvents = false drops pointer events on Client A
  rfb::Server::acceptPointerEvents.setParam(false);
  clientA.sendGIIEvent(spoof);
  pumpPair(&clientA, cSockA, &server, sSockA);
  EXPECT_TRUE(desktop.events.empty());

  server.removeSocket(sSockC);
  server.removeSocket(sSockB);
  server.removeSocket(sSockA);
}

// ---------------------------------------------------------------------------
// 3. vncviewer GIIDeviceManager & Classification Tests
// ---------------------------------------------------------------------------

static RawInputDeviceInfo makeStylusRaw(int id, const char* name)
{
  RawInputDeviceInfo raw;
  raw.localId = id;
  raw.name = name;
  raw.enabled = true;
  raw.isSlavePointer = true;
  raw.vendorID = 0x056a;
  raw.productID = 0x0357;
  raw.numButtons = 3;

  RawInputAxisInfo axX;
  axX.number = 0;
  axX.label = "Abs X";
  axX.minVal = 0.0;
  axX.maxVal = 30000.0;
  axX.isAbsolute = true;
  raw.axes.push_back(axX);

  RawInputAxisInfo axY;
  axY.number = 1;
  axY.label = "Abs Y";
  axY.minVal = 0.0;
  axY.maxVal = 20000.0;
  axY.isAbsolute = true;
  raw.axes.push_back(axY);

  RawInputAxisInfo axPrs;
  axPrs.number = 2;
  axPrs.label = "Abs Pressure";
  axPrs.minVal = 0.0;
  axPrs.maxVal = 8191.0;
  axPrs.isAbsolute = true;
  raw.axes.push_back(axPrs);

  return raw;
}

TEST(GIIDeviceManager, DeviceClassification)
{
  rfb::GIIDevice desc;
  std::vector<int> axisNums;
  std::vector<bool> axisAbs;
  int xIdx = -1, yIdx = -1;

  // Accepted: Wacom stylus
  EXPECT_TRUE(classifyAndBuildGIIDevice(
    makeStylusRaw(10, "Wacom Intuos Pro Pen stylus"),
    &desc, &axisNums, &axisAbs, &xIdx, &yIdx));
  EXPECT_EQ(xIdx, 0);
  EXPECT_EQ(yIdx, 1);
  ASSERT_EQ(desc.valuators.size(), 3U);
  EXPECT_EQ(desc.valuators[2].shortName, "Prs");
  EXPECT_EQ(desc.valuators[2].siDiv, 1);

  // Accepted: xwayland-tablet stylus
  EXPECT_TRUE(classifyAndBuildGIIDevice(
    makeStylusRaw(11, "xwayland-tablet-stylus:1"),
    &desc, &axisNums, &axisAbs, &xIdx, &yIdx));

  // Accepted: Floating Wacom Pad with buttons only
  RawInputDeviceInfo pad;
  pad.localId = 12;
  pad.name = "Wacom Intuos Pro Pad pad";
  pad.enabled = true;
  pad.isFloatingSlave = true;
  pad.numButtons = 9;
  EXPECT_TRUE(classifyAndBuildGIIDevice(
    pad, &desc, &axisNums, &axisAbs, &xIdx, &yIdx));
  EXPECT_EQ(xIdx, -1);
  EXPECT_EQ(yIdx, -1);
  EXPECT_EQ(desc.numButtons, 9U);

  // Accepted: 6-DOF SpaceMouse
  RawInputDeviceInfo spaceMouse;
  spaceMouse.localId = 13;
  spaceMouse.name = "3Dconnexion SpaceMouse Compact";
  spaceMouse.enabled = true;
  spaceMouse.isSlavePointer = true;
  spaceMouse.numButtons = 2;
  const char* smLabels[6] = {"Rel X", "Rel Y", "Rel Z",
                             "Rel Rot X", "Rel Rot Y", "Rel Rot Z"};
  for (int i = 0; i < 6; i++) {
    RawInputAxisInfo ax;
    ax.number = i;
    ax.label = smLabels[i];
    ax.minVal = -500;
    ax.maxVal = 500;
    ax.isAbsolute = false;
    spaceMouse.axes.push_back(ax);
  }
  EXPECT_TRUE(classifyAndBuildGIIDevice(
    spaceMouse, &desc, &axisNums, &axisAbs, &xIdx, &yIdx));
  EXPECT_EQ(desc.valuators.size(), 6U);
  EXPECT_TRUE(desc.canGenerate & rfb::giiEventMaskValuatorRelative);

  // Rejected: regular 2-axis mouse (+ scroll axes ignored)
  RawInputDeviceInfo mouse;
  mouse.localId = 20;
  mouse.name = "Logitech USB Optical Mouse";
  mouse.enabled = true;
  mouse.isSlavePointer = true;
  mouse.numButtons = 5;
  {
    RawInputAxisInfo a0; a0.number = 0; a0.label = "Rel X";
    RawInputAxisInfo a1; a1.number = 1; a1.label = "Rel Y";
    RawInputAxisInfo a2; a2.number = 2; a2.label = "Rel Vert Wheel"; a2.isScroll = true;
    mouse.axes.push_back(a0);
    mouse.axes.push_back(a1);
    mouse.axes.push_back(a2);
  }
  EXPECT_FALSE(classifyAndBuildGIIDevice(
    mouse, &desc, &axisNums, &axisAbs, &xIdx, &yIdx));

  mouse.name = "ThinkPad USB Mouse";
  EXPECT_FALSE(classifyAndBuildGIIDevice(
    mouse, &desc, &axisNums, &axisAbs, &xIdx, &yIdx));

  // Rejected: XTEST, xwayland-pointer, touchpad, touchscreen, QEMU USB Tablet
  RawInputDeviceInfo xtest = makeStylusRaw(21, "Virtual core XTEST pointer");
  EXPECT_FALSE(classifyAndBuildGIIDevice(
    xtest, &desc, &axisNums, &axisAbs, &xIdx, &yIdx));

  RawInputDeviceInfo xwp = makeStylusRaw(22, "xwayland-pointer:17");
  EXPECT_FALSE(classifyAndBuildGIIDevice(
    xwp, &desc, &axisNums, &axisAbs, &xIdx, &yIdx));

  RawInputDeviceInfo touchpad;
  touchpad.localId = 23;
  touchpad.name = "SynPS/2 Synaptics TouchPad";
  touchpad.enabled = true;
  touchpad.isSlavePointer = true;
  touchpad.hasTouch = true;
  touchpad.isDirectTouch = false;
  touchpad.axes.push_back(mouse.axes[0]);
  touchpad.axes.push_back(mouse.axes[1]);
  EXPECT_FALSE(classifyAndBuildGIIDevice(
    touchpad, &desc, &axisNums, &axisAbs, &xIdx, &yIdx));

  RawInputDeviceInfo touchScreen = touchpad;
  touchScreen.name = "ELAN Touchscreen";
  touchScreen.isDirectTouch = true;
  EXPECT_FALSE(classifyAndBuildGIIDevice(
    touchScreen, &desc, &axisNums, &axisAbs, &xIdx, &yIdx));

  RawInputDeviceInfo qemuTablet;
  qemuTablet.localId = 24;
  qemuTablet.name = "QEMU QEMU USB Tablet";
  qemuTablet.enabled = true;
  qemuTablet.isSlavePointer = true;
  qemuTablet.axes.push_back(makeStylusRaw(24, "").axes[0]);
  qemuTablet.axes.push_back(makeStylusRaw(24, "").axes[1]);
  EXPECT_FALSE(classifyAndBuildGIIDevice(
    qemuTablet, &desc, &axisNums, &axisAbs, &xIdx, &yIdx));
}

TEST(GIIDeviceManager, LifecycleScalingAndEventBatching)
{
  MockGIIEventSender sender;
  GIIDeviceManager mgr(&sender);

  rfb::GIIDevice desc;
  std::vector<int> axisNums;
  std::vector<bool> axisAbs;
  int xIdx = -1, yIdx = -1;
  ASSERT_TRUE(classifyAndBuildGIIDevice(
    makeStylusRaw(10, "Wacom Stylus"),
    &desc, &axisNums, &axisAbs, &xIdx, &yIdx));

  // Coordinate scaling checks
  EXPECT_EQ(scaleCoordinate(-5.0, 1001, 0, 30000), 0);
  EXPECT_EQ(scaleCoordinate(500.0, 1001, 0, 30000), 15000);
  EXPECT_EQ(scaleCoordinate(1000.0, 1001, 0, 30000), 30000);
  EXPECT_EQ(scaleCoordinate(1500.0, 1001, 0, 30000), 30000);

  mgr.addDevice(10, false, desc, axisNums, axisAbs, xIdx, yIdx);
  mgr.addDevice(11, false, desc, axisNums, axisAbs, xIdx, yIdx);
  ASSERT_EQ(sender.createdRequests.size(), 2U);

  // Remove 11 while creation is still pending, then re-add 11
  mgr.removeDevice(11);
  mgr.addDevice(11, false, desc, axisNums, axisAbs, xIdx, yIdx);

  // Server replies for: 10 (origin 101), stale 11 (origin 102), new 11 (origin 103)
  mgr.handleDeviceCreated(101);
  mgr.handleDeviceCreated(102);
  mgr.handleDeviceCreated(103);

  EXPECT_TRUE(mgr.isActiveDevice(10));
  EXPECT_TRUE(mgr.isActiveDevice(11));
  ASSERT_EQ(sender.destroyedOrigins.size(), 1U);
  EXPECT_EQ(sender.destroyedOrigins[0], 102U);

  // Motion event with X, Y, Pressure -> contiguous single valuator event
  std::vector<std::pair<int, double>> vals;
  vals.push_back(std::make_pair(0, 1234.0));
  vals.push_back(std::make_pair(1, 5678.0));
  vals.push_back(std::make_pair(2, 4096.0));
  EXPECT_TRUE(mgr.handleInputEvent(10, GIIDeviceManager::EventMotion, 0,
                                   500.0, 250.0, 1001, 501, vals));

  ASSERT_EQ(sender.sentBatches.size(), 1U);
  ASSERT_EQ(sender.sentBatches[0].size(), 1U);
  EXPECT_EQ(sender.sentBatches[0][0].type, rfb::giiEventValuatorAbsolute);
  EXPECT_EQ(sender.sentBatches[0][0].origin, 101U);
  EXPECT_EQ(sender.sentBatches[0][0].first, 0U);
  ASSERT_EQ(sender.sentBatches[0][0].values.size(), 3U);
  EXPECT_EQ(sender.sentBatches[0][0].values[0], 15000);
  EXPECT_EQ(sender.sentBatches[0][0].values[1], 10000);
  EXPECT_EQ(sender.sentBatches[0][0].values[2], 4096);

  // Button press with no axes -> emits X/Y valuators + ButtonPress
  vals.clear();
  EXPECT_TRUE(mgr.handleInputEvent(10, GIIDeviceManager::EventButtonPress, 1,
                                   100.0, 50.0, 1001, 501, vals));
  EXPECT_TRUE(mgr.hasPressedButtons(10));
  ASSERT_EQ(sender.sentBatches.size(), 2U);
  ASSERT_EQ(sender.sentBatches[1].size(), 2U);
  EXPECT_EQ(sender.sentBatches[1][0].type, rfb::giiEventValuatorAbsolute);
  EXPECT_EQ(sender.sentBatches[1][1].type, rfb::giiEventPtrButtonPress);
  EXPECT_EQ(sender.sentBatches[1][1].button, 1U);

  // releaseAllButtons() releases button 1
  mgr.releaseAllButtons();
  EXPECT_FALSE(mgr.hasPressedButtons(10));
  ASSERT_EQ(sender.sentBatches.size(), 3U);
  EXPECT_EQ(sender.sentBatches[2][0].type, rfb::giiEventPtrButtonRelease);

  // A release for a press handled before GII activation can fall back to RFB.
  EXPECT_FALSE(mgr.handleInputEvent(10, GIIDeviceManager::EventButtonRelease, 1,
                                    100.0, 50.0, 1001, 501, vals));
  EXPECT_EQ(sender.sentBatches.size(), 3U);

  // destroyAllDevices() destroys 101 and 103
  mgr.destroyAllDevices();
  EXPECT_FALSE(mgr.hasDevice(10));
  EXPECT_FALSE(mgr.hasDevice(11));
  ASSERT_EQ(sender.destroyedOrigins.size(), 3U);
}

TEST(GIIDeviceManager, LateDeviceCreationAfterDisable)
{
  MockGIIEventSender sender;
  GIIDeviceManager mgr(&sender);
  rfb::GIIDevice desc;
  desc.name = "Identical Name for Multiple Tablet Devices";

  mgr.addDevice(10, false, desc, {}, {}, -1, -1);
  mgr.addDevice(11, false, desc, {}, {}, -1, -1);
  ASSERT_EQ(sender.createdRequests.size(), 2U);
  EXPECT_NE(sender.createdRequests[0].name, sender.createdRequests[1].name);
  mgr.destroyAllDevices();

  mgr.addDevice(11, false, desc, {}, {}, -1, -1);
  ASSERT_EQ(sender.createdRequests.size(), 3U);
  EXPECT_NE(sender.createdRequests[2].name, sender.createdRequests[0].name);
  EXPECT_NE(sender.createdRequests[2].name, sender.createdRequests[1].name);
  EXPECT_LE(sender.createdRequests[2].name.size(), rfb::giiDeviceNameLen - 1);

  mgr.handleDeviceCreated(100);
  mgr.handleDeviceCreated(101);
  EXPECT_FALSE(mgr.isActiveDevice(11));
  mgr.handleDeviceCreated(102);
  EXPECT_TRUE(mgr.isActiveDevice(11));

  mgr.addDevice(12, false, desc, {}, {}, -1, -1);
  mgr.handleDeviceCreated(102);
  EXPECT_FALSE(mgr.isActiveDevice(12));
  EXPECT_TRUE(mgr.isActiveDevice(11));

  mgr.handleDeviceCreated(102);
  EXPECT_EQ(sender.destroyedOrigins, (std::vector<uint32_t>{100, 101}));
}
