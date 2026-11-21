/* Copyright (C) 2026 TigerVNC Team
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
#include <zlib.h>

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <rdr/InStream.h>
#include <rdr/MemInStream.h>
#include <rdr/MemOutStream.h>
#include <rfb/CConnection.h>
#include <rfb/CMsgReader.h>
#include <rfb/CMsgWriter.h>
#include <rfb/CSecurityTight.h>
#include <rfb/Exception.h>
#include <rfb/FileTransferClient.h>
#include <rfb/PixelBuffer.h>
#include <rfb/TightCapability.h>
#include <rfb/encodings.h>
#include <rfb/fileTransferTypes.h>
#include <rfb/msgTypes.h>

namespace {

class ChunkedInStream : public rdr::InStream {
public:
  ChunkedInStream() : limit_(0)
  {
    ptr = end = nullptr;
  }

  void setBuffer(const std::vector<uint8_t>& buf)
  {
    data_ = buf;
    limit_ = 0;
    ptr = data_.data();
    end = data_.data();
  }

  void expose(size_t extra)
  {
    limit_ = std::min(data_.size(), limit_ + extra);
    end = data_.data() + limit_;
  }

  void exposeAll()
  {
    limit_ = data_.size();
    end = data_.data() + limit_;
  }

  size_t pos() override
  {
    return data_.empty() ? 0 : static_cast<size_t>(ptr - data_.data());
  }

private:
  bool overrun(size_t needed) override
  {
    return static_cast<size_t>(end - ptr) >= needed;
  }

  std::vector<uint8_t> data_;
  size_t limit_;
};

static void appendU8(std::vector<uint8_t>* out, uint8_t v)
{
  out->push_back(v);
}

static void appendU16BE(std::vector<uint8_t>* out, uint16_t v)
{
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(v & 0xFF));
}

static void appendU32BE(std::vector<uint8_t>* out, uint32_t v)
{
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(v & 0xFF));
}

static void appendU64BE(std::vector<uint8_t>* out, uint64_t v)
{
  appendU32BE(out, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFU));
  appendU32BE(out, static_cast<uint32_t>(v & 0xFFFFFFFFU));
}

static void appendU16LE(std::vector<uint8_t>* out, uint16_t v)
{
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

static void appendCap(std::vector<uint8_t>* out, int32_t code,
                      const char vendor[4], const char sig[8])
{
  appendU32BE(out, static_cast<uint32_t>(code));
  out->insert(out->end(), vendor, vendor + 4);
  out->insert(out->end(), sig, sig + 8);
}

static std::vector<uint8_t> compressZSync(const uint8_t* data, size_t len,
                                          int windowBits = 15)
{
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  EXPECT_EQ(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         windowBits, 8, Z_DEFAULT_STRATEGY), Z_OK);
  std::vector<uint8_t> out(deflateBound(&zs, static_cast<uLong>(len)) + 32);
  zs.next_in = const_cast<Bytef*>(data);
  zs.avail_in = static_cast<uInt>(len);
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());
  EXPECT_EQ(deflate(&zs, Z_SYNC_FLUSH), Z_OK);
  size_t written = zs.total_out;
  deflateEnd(&zs);
  out.resize(written);
  return out;
}

class RecordingFTHandler : public rfb::FileTransferHandler {
public:
  int readyCount = 0;
  rfb::FileTransferCaps lastCaps;

  int listCount = 0;
  bool lastListOk = false;
  std::vector<rfb::FileEntry> lastEntries;
  std::string lastListErr;

  int downloadStartedCount = 0;
  bool lastDownloadStartedOk = false;
  uint64_t lastDownloadTotal = 0;
  bool lastDownloadSizeKnown = false;
  std::string lastDownloadErr;
  std::vector<uint8_t> downloadedBytes;
  int downloadCompleteCount = 0;
  bool lastDownloadCompleteOk = false;

  int uploadReadyCount = 0;
  bool lastUploadReadyOk = false;
  std::string lastUploadErr;
  int uploadAckCount = 0;
  int uploadCompleteCount = 0;
  bool lastUploadCompleteOk = false;

  int cmdCompleteCount = 0;
  bool lastCmdOk = false;
  std::string lastCmdErr;

  int cancelCompleteCount = 0;

  void onFileTransferReady(const rfb::FileTransferCaps& caps) override
  {
    readyCount++;
    lastCaps = caps;
  }

  void onListComplete(bool ok, const std::vector<rfb::FileEntry>& entries,
                      const std::string& error) override
  {
    listCount++;
    lastListOk = ok;
    lastEntries = entries;
    lastListErr = error;
  }

  void onDownloadStarted(bool ok, uint64_t totalSize, bool sizeKnown,
                         const std::string& error) override
  {
    downloadStartedCount++;
    lastDownloadStartedOk = ok;
    lastDownloadTotal = totalSize;
    lastDownloadSizeKnown = sizeKnown;
    lastDownloadErr = error;
  }

  void onDownloadData(const uint8_t* data, size_t len) override
  {
    if (data != nullptr && len > 0)
      downloadedBytes.insert(downloadedBytes.end(), data, data + len);
  }

  void onDownloadComplete(bool ok, const std::string& error) override
  {
    downloadCompleteCount++;
    lastDownloadCompleteOk = ok;
    lastDownloadErr = error;
  }

  void onUploadReady(bool ok, const std::string& error) override
  {
    uploadReadyCount++;
    lastUploadReadyOk = ok;
    lastUploadErr = error;
  }

  void onUploadChunkAck() override
  {
    uploadAckCount++;
  }

  void onUploadComplete(bool ok, const std::string& error) override
  {
    uploadCompleteCount++;
    lastUploadCompleteOk = ok;
    lastUploadErr = error;
  }

  void onCommandComplete(bool ok, const std::string& error) override
  {
    cmdCompleteCount++;
    lastCmdOk = ok;
    lastCmdErr = error;
  }

  void onCancelComplete() override
  {
    cancelCompleteCount++;
  }
};

class TestCConnection : public rfb::CConnection {
public:
  TestCConnection()
  {
    supportsFileTransfer = true;
  }

  void initDone() override
  {
    setFramebuffer(new rfb::ManagedPixelBuffer(rfb::PixelFormat(),
                                               server.width(),
                                               server.height()));
  }
  void bell() override {}

  void getUserPasswd(std::string* user, std::string* password) override
  {
    if (user != nullptr)
      *user = "alice";
    if (password != nullptr)
      *password = "secret12";
  }

  bool verifyCertificate(unsigned int /*status*/,
                         const uint8_t* /*certificate*/,
                         size_t /*length*/) override
  {
    return true;
  }

  bool verifyHostKey(const uint8_t* /*key*/, size_t /*length*/,
                     const char* /*fingerprint*/) override
  {
    return true;
  }
};

} // namespace

TEST(FileTransferUnit, CSecurityTight_NoTunnelNoAuth_Incremental)
{
  TestCConnection conn;
  rdr::MemOutStream out;
  ChunkedInStream in;

  std::vector<uint8_t> serverBytes;
  appendU32BE(&serverBytes, 1);
  appendCap(&serverBytes, rfb::tightTunnelNone, rfb::tightVendorTight,
            rfb::sigTightTunnelNone);
  appendU32BE(&serverBytes, 1);
  appendCap(&serverBytes, rfb::tightAuthNone, rfb::tightVendorStandard,
            rfb::sigTightAuthNone);

  in.setBuffer(serverBytes);
  conn.setStreams(&in, &out);

  rfb::SecurityClient sec;
  rfb::CSecurityTight tight(&conn, &sec);

  EXPECT_EQ(tight.getType(), rfb::secTypeTight);

  bool done = false;
  for (size_t i = 0; i < serverBytes.size() && !done; i++) {
    in.expose(1);
    done = tight.processMsg();
  }
  if (!done)
    done = tight.processMsg();

  EXPECT_TRUE(done);
  EXPECT_EQ(tight.getEffectiveType(), rfb::secTypeNone);
  ASSERT_EQ(out.length(), 8U);
  const uint8_t* p = out.data();
  EXPECT_EQ(p[3], 0U); // tightTunnelNone
  EXPECT_EQ(p[7], 1U); // tightAuthNone
}

TEST(FileTransferUnit, CSecurityTight_VncAuth)
{
  TestCConnection conn;
  rdr::MemOutStream out;
  ChunkedInStream in;

  std::vector<uint8_t> serverBytes;
  appendU32BE(&serverBytes, 1);
  appendCap(&serverBytes, rfb::tightTunnelNone, rfb::tightVendorTight,
            rfb::sigTightTunnelNone);
  appendU32BE(&serverBytes, 2);
  appendCap(&serverBytes, rfb::tightAuthNone, rfb::tightVendorStandard,
            rfb::sigTightAuthNone);
  appendCap(&serverBytes, rfb::tightAuthVnc, rfb::tightVendorStandard,
            rfb::sigTightAuthVnc);
  // 16-byte VncAuth challenge
  for (int i = 0; i < 16; i++)
    serverBytes.push_back(static_cast<uint8_t>(i + 1));

  in.setBuffer(serverBytes);
  conn.setStreams(&in, &out);

  rfb::SecurityClient sec;
  std::list<uint32_t> onlyVnc = { rfb::secTypeTight, rfb::secTypeVncAuth };
  sec.SetSecTypes(onlyVnc);

  rfb::CSecurityTight tight(&conn, &sec);

  bool done = false;
  for (size_t i = 0; i < serverBytes.size() && !done; i++) {
    in.expose(1);
    done = tight.processMsg();
  }

  EXPECT_TRUE(done);
  EXPECT_EQ(tight.getEffectiveType(), rfb::secTypeVncAuth);
  // 4-byte tunnel + 4-byte auth + 16-byte VncAuth challenge response = 24 bytes
  EXPECT_EQ(out.length(), 24U);
  EXPECT_EQ(out.data()[7], 2U); // tightAuthVnc
}

TEST(FileTransferUnit, CSecurityTight_RejectsUnsupportedTunnel)
{
  TestCConnection conn;
  rdr::MemOutStream out;
  std::vector<uint8_t> serverBytes;
  appendU32BE(&serverBytes, 1);
  appendCap(&serverBytes, 99, "TUVN", "BADTUNNL");

  rdr::MemInStream in(serverBytes.data(), serverBytes.size());
  conn.setStreams(&in, &out);

  rfb::SecurityClient sec;
  rfb::CSecurityTight tight(&conn, &sec);
  EXPECT_THROW(tight.processMsg(), rfb::protocol_error);
}

TEST(FileTransferUnit, ReadServerInitTightInteractionCaps_Incremental)
{
  TestCConnection conn;
  rdr::MemOutStream out;
  RecordingFTHandler ftHandler;
  conn.fileTransferClient().setHandler(&ftHandler);
  conn.server.tightEnabled = true;

  std::vector<uint8_t> serverInit;
  appendU16BE(&serverInit, 64);
  appendU16BE(&serverInit, 64);
  // 16-byte PixelFormat
  rdr::MemOutStream pfOs;
  rfb::PixelFormat().write(&pfOs);
  serverInit.insert(serverInit.end(), pfOs.data(), pfOs.data() + pfOs.length());
  // Desktop name "TightBox"
  const char* name = "TightBox";
  appendU32BE(&serverInit, static_cast<uint32_t>(strlen(name)));
  serverInit.insert(serverInit.end(), name, name + strlen(name));

  // InteractionCaps counts: 3 serverMsgs, 2 clientMsgs, 0 encodings
  appendU16BE(&serverInit, 3);
  appendU16BE(&serverInit, 2);
  appendU16BE(&serverInit, 0);
  appendU16BE(&serverInit, 0); // padding

  appendCap(&serverInit, rfb::tight2CompressionSupportReply,
            rfb::tightVendorTight, rfb::sigTight2CompressionSupportReply);
  appendCap(&serverInit, rfb::tight2FileListReply,
            rfb::tightVendorTight, rfb::sigTight2FileListReply);
  appendCap(&serverInit, rfb::tight2LastRequestFailedReply,
            rfb::tightVendorTight, rfb::sigTight2LastRequestFailedReply);

  appendCap(&serverInit, rfb::tight2CompressionSupportReq,
            rfb::tightVendorTight, rfb::sigTight2CompressionSupportReq);
  appendCap(&serverInit, rfb::tight2FileListReq,
            rfb::tightVendorTight, rfb::sigTight2FileListReq);

  ChunkedInStream in;
  in.setBuffer(serverInit);
  conn.setStreams(&in, &out);
  rfb::CMsgReader reader(&conn, &in);

  bool readDone = false;
  for (size_t i = 0; i < serverInit.size() && !readDone; i++) {
    in.expose(1);
    readDone = reader.readServerInit();
  }
  EXPECT_TRUE(readDone);

  // CConnection::serverInit() calls updateEncodings() and fileTransfer.onServerInit()
  EXPECT_GT(out.length(), 0U);
  EXPECT_FALSE(conn.fileTransferClient().isSupported());

  std::vector<uint8_t> compReply;
  appendU32BE(&compReply, rfb::tight2CompressionSupportReply);
  appendU8(&compReply, 1);
  rdr::MemInStream repIn(compReply.data(), compReply.size());
  rfb::CMsgReader repReader(&conn, &repIn);
  EXPECT_TRUE(repReader.readMsg());

  EXPECT_EQ(ftHandler.readyCount, 1);
  EXPECT_TRUE(conn.fileTransferClient().isSupported());
  EXPECT_EQ(ftHandler.lastCaps.protocol, rfb::FileTransferProtocol::Tight2);
  EXPECT_TRUE(ftHandler.lastCaps.canCompress);
  EXPECT_TRUE(ftHandler.lastCaps.canList);
}

TEST(FileTransferUnit, Tight2WireRoundTrip)
{
  TestCConnection conn;
  RecordingFTHandler ftHandler;
  conn.fileTransferClient().setHandler(&ftHandler);
  conn.server.tightEnabled = true;

  rdr::MemOutStream clientOut;
  rfb::CMsgWriter clientWriter(&conn.server, &clientOut);
  conn.fileTransferClient().setWriter(&clientWriter);

  std::vector<rfb::TightCapability> sCaps;
  auto pushS = [&](uint32_t code, const char* sig) {
    rfb::TightCapability c;
    c.code = static_cast<int32_t>(code);
    memcpy(c.vendor, rfb::tightVendorTight, 4);
    memcpy(c.signature, sig, 8);
    sCaps.push_back(c);
  };
  pushS(rfb::tight2CompressionSupportReply, rfb::sigTight2CompressionSupportReply);
  pushS(rfb::tight2FileListReply, rfb::sigTight2FileListReply);
  pushS(rfb::tight2UploadStartReply, rfb::sigTight2UploadStartReply);
  pushS(rfb::tight2UploadDataReply, rfb::sigTight2UploadDataReply);
  pushS(rfb::tight2UploadEndReply, rfb::sigTight2UploadEndReply);
  pushS(rfb::tight2DownloadStartReply, rfb::sigTight2DownloadStartReply);
  pushS(rfb::tight2DownloadDataReply, rfb::sigTight2DownloadDataReply);
  pushS(rfb::tight2DownloadEndReply, rfb::sigTight2DownloadEndReply);
  pushS(rfb::tight2MkdirReply, rfb::sigTight2MkdirReply);
  pushS(rfb::tight2RemoveReply, rfb::sigTight2RemoveReply);
  pushS(rfb::tight2RenameReply, rfb::sigTight2RenameReply);
  pushS(rfb::tight2LastRequestFailedReply, rfb::sigTight2LastRequestFailedReply);

  std::vector<rfb::TightCapability> cCaps;
  auto pushC = [&](uint32_t code, const char* sig) {
    rfb::TightCapability c;
    c.code = static_cast<int32_t>(code);
    memcpy(c.vendor, rfb::tightVendorTight, 4);
    memcpy(c.signature, sig, 8);
    cCaps.push_back(c);
  };
  pushC(rfb::tight2CompressionSupportReq, rfb::sigTight2CompressionSupportReq);
  pushC(rfb::tight2FileListReq, rfb::sigTight2FileListReq);
  pushC(rfb::tight2UploadStartReq, rfb::sigTight2UploadStartReq);
  pushC(rfb::tight2UploadDataReq, rfb::sigTight2UploadDataReq);
  pushC(rfb::tight2UploadEndReq, rfb::sigTight2UploadEndReq);
  pushC(rfb::tight2DownloadStartReq, rfb::sigTight2DownloadStartReq);
  pushC(rfb::tight2DownloadDataReq, rfb::sigTight2DownloadDataReq);
  pushC(rfb::tight2MkdirReq, rfb::sigTight2MkdirReq);
  pushC(rfb::tight2RemoveReq, rfb::sigTight2RemoveReq);
  pushC(rfb::tight2RenameReq, rfb::sigTight2RenameReq);

  conn.fileTransferClient().onServerTightCaps(sCaps, cCaps, {});
  conn.fileTransferClient().onServerInit();
  conn.fileTransferClient().onTight2CompressionSupportReply(true);
  ASSERT_EQ(ftHandler.readyCount, 1);

  // Compressed Tight2 FILE_LIST_REPLY using Z_SYNC_FLUSH (raw windowBits=-15)
  std::vector<uint8_t> listPayload;
  appendU32BE(&listPayload, 2); // 2 entries
  // Entry 0: "." (filtered out by FileTransferClient)
  appendU64BE(&listPayload, 0);
  appendU64BE(&listPayload, 1700000000000ULL);
  appendU16BE(&listPayload, rfb::tight2FlagDir);
  appendU32BE(&listPayload, 1);
  listPayload.push_back('.');
  // Entry 1: "data.bin"
  appendU64BE(&listPayload, 4096);
  appendU64BE(&listPayload, 1700000000000ULL);
  appendU16BE(&listPayload, rfb::tight2FlagExec);
  const char* fname = "data.bin";
  appendU32BE(&listPayload, static_cast<uint32_t>(strlen(fname)));
  listPayload.insert(listPayload.end(), fname, fname + strlen(fname));

  std::vector<uint8_t> compList = compressZSync(listPayload.data(),
                                                listPayload.size(), -15);
  std::vector<uint8_t> wireList;
  appendU32BE(&wireList, rfb::tight2FileListReply);
  appendU8(&wireList, 1);
  appendU32BE(&wireList, static_cast<uint32_t>(compList.size()));
  appendU32BE(&wireList, static_cast<uint32_t>(listPayload.size()));
  wireList.insert(wireList.end(), compList.begin(), compList.end());

  conn.fileTransferClient().listDirectory("/");
  ChunkedInStream inList;
  inList.setBuffer(wireList);
  rfb::CMsgReader rdrList(&conn, &inList);
  bool got = false;
  for (size_t i = 0; i < wireList.size() && !got; i++) {
    inList.expose(1);
    got = rdrList.readMsg();
  }
  EXPECT_TRUE(got);
  ASSERT_EQ(ftHandler.listCount, 1);
  EXPECT_TRUE(ftHandler.lastListOk);
  ASSERT_EQ(ftHandler.lastEntries.size(), 1U);
  EXPECT_EQ(ftHandler.lastEntries[0].name, "data.bin");
  EXPECT_EQ(ftHandler.lastEntries[0].size, 4096U);
  EXPECT_TRUE(ftHandler.lastEntries[0].isExecutable);

  // Compressed Tight2 download (windowBits=15 Z_SYNC_FLUSH)
  const std::string dlText = "Hello TightVNC 2.x FileTransfer payload!";
  std::vector<uint8_t> compDl = compressZSync(
    reinterpret_cast<const uint8_t*>(dlText.data()), dlText.size(), 15);
  std::vector<uint8_t> wireDl;
  appendU32BE(&wireDl, rfb::tight2DownloadStartReply);
  appendU32BE(&wireDl, rfb::tight2DownloadDataReply);
  appendU8(&wireDl, 1);
  appendU32BE(&wireDl, static_cast<uint32_t>(compDl.size()));
  appendU32BE(&wireDl, static_cast<uint32_t>(dlText.size()));
  wireDl.insert(wireDl.end(), compDl.begin(), compDl.end());
  appendU32BE(&wireDl, rfb::tight2DownloadEndReply);
  appendU8(&wireDl, 0);
  appendU64BE(&wireDl, 1700000000000ULL);

  conn.fileTransferClient().startDownload("/data.bin", 0);
  rdr::MemInStream dlIn(wireDl.data(), wireDl.size());
  rfb::CMsgReader rdrDl(&conn, &dlIn);
  EXPECT_TRUE(rdrDl.readMsg()); // StartReply
  conn.fileTransferClient().requestMoreDownloadData();
  EXPECT_TRUE(rdrDl.readMsg()); // DataReply
  EXPECT_TRUE(rdrDl.readMsg()); // EndReply

  EXPECT_EQ(ftHandler.downloadStartedCount, 1);
  EXPECT_EQ(ftHandler.downloadCompleteCount, 1);
  EXPECT_TRUE(ftHandler.lastDownloadCompleteOk);
  EXPECT_EQ(std::string(ftHandler.downloadedBytes.begin(),
                        ftHandler.downloadedBytes.end()), dlText);

  // Upload pre-start vs mid-transfer LastRequestFailed distinction
  std::vector<uint8_t> failMsg;
  const char* errStr = "Disk full";
  appendU32BE(&failMsg, rfb::tight2LastRequestFailedReply);
  appendU32BE(&failMsg, static_cast<uint32_t>(strlen(errStr)));
  failMsg.insert(failMsg.end(), errStr, errStr + strlen(errStr));

  // Case 1: Pre-start failure -> onUploadReady(false)
  conn.fileTransferClient().startUpload("/up.bin", true, 0);
  {
    rdr::MemInStream failIn(failMsg.data(), failMsg.size());
    rfb::CMsgReader r(&conn, &failIn);
    EXPECT_TRUE(r.readMsg());
  }
  EXPECT_EQ(ftHandler.uploadReadyCount, 1);
  EXPECT_FALSE(ftHandler.lastUploadReadyOk);
  EXPECT_EQ(ftHandler.uploadCompleteCount, 0);

  // Case 2: Post-start failure -> onUploadReady(true) then onUploadComplete(false)
  conn.fileTransferClient().startUpload("/up.bin", true, 0);
  std::vector<uint8_t> startOk;
  appendU32BE(&startOk, rfb::tight2UploadStartReply);
  {
    rdr::MemInStream sIn(startOk.data(), startOk.size());
    rfb::CMsgReader r(&conn, &sIn);
    EXPECT_TRUE(r.readMsg());
  }
  EXPECT_EQ(ftHandler.uploadReadyCount, 2);
  EXPECT_TRUE(ftHandler.lastUploadReadyOk);

  const uint8_t chunk[4] = {1, 2, 3, 4};
  conn.fileTransferClient().sendUploadData(chunk, sizeof(chunk));
  {
    rdr::MemInStream failIn(failMsg.data(), failMsg.size());
    rfb::CMsgReader r(&conn, &failIn);
    EXPECT_TRUE(r.readMsg());
  }
  EXPECT_EQ(ftHandler.uploadCompleteCount, 1);
  EXPECT_FALSE(ftHandler.lastUploadCompleteOk);
  EXPECT_EQ(ftHandler.lastUploadErr, "Disk full");
}

TEST(FileTransferUnit, Tight13WireRoundTrip)
{
  TestCConnection conn;
  RecordingFTHandler ftHandler;
  conn.fileTransferClient().setHandler(&ftHandler);
  conn.server.tightEnabled = true;

  rdr::MemOutStream clientOut;
  rfb::CMsgWriter clientWriter(&conn.server, &clientOut);
  conn.fileTransferClient().setWriter(&clientWriter);

  std::vector<rfb::TightCapability> sCaps;
  auto pushS = [&](int32_t code, const char* sig) {
    rfb::TightCapability c;
    c.code = code;
    memcpy(c.vendor, rfb::tightVendorTight, 4);
    memcpy(c.signature, sig, 8);
    sCaps.push_back(c);
  };
  pushS(rfb::tight13CapFileListData, rfb::sigTight13FileListData);
  pushS(rfb::tight13CapFileDownloadData, rfb::sigTight13FileDownloadData);
  pushS(rfb::tight13CapFileUploadCancel, rfb::sigTight13FileUploadCancel);
  pushS(rfb::tight13CapFileDownloadFailed, rfb::sigTight13FileDownloadFailed);

  std::vector<rfb::TightCapability> cCaps;
  auto pushC = [&](int32_t code, const char* sig) {
    rfb::TightCapability c;
    c.code = code;
    memcpy(c.vendor, rfb::tightVendorTight, 4);
    memcpy(c.signature, sig, 8);
    cCaps.push_back(c);
  };
  pushC(rfb::tight13CapFileListReq, rfb::sigTight13FileListReq);
  pushC(rfb::tight13CapFileDownloadReq, rfb::sigTight13FileDownloadReq);
  pushC(rfb::tight13CapFileUploadReq, rfb::sigTight13FileUploadReq);
  pushC(rfb::tight13CapFileUploadData, rfb::sigTight13FileUploadData);
  pushC(rfb::tight13CapFileDownloadCancel, rfb::sigTight13FileDownloadCancel);
  pushC(rfb::tight13CapFileUploadFailed, rfb::sigTight13FileUploadFailed);
  // Deliberately omit FTC_FCDR like LibVNCServer

  conn.fileTransferClient().onServerTightCaps(sCaps, cCaps, {});
  conn.fileTransferClient().onServerInit();
  EXPECT_TRUE(conn.fileTransferClient().isSupported());
  EXPECT_TRUE(ftHandler.lastCaps.canMkdir);

  // Compressed Tight13 FileListData
  const std::string rawNames = std::string("dirA\0fileB.txt\0", 15);
  std::vector<uint8_t> compNames = compressZSync(
    reinterpret_cast<const uint8_t*>(rawNames.data()), rawNames.size(), 15);
  std::vector<uint8_t> listWire;
  appendU8(&listWire, rfb::msgTypeTightFileListData);
  appendU8(&listWire, 0x01); // compressed level 1
  appendU16BE(&listWire, 2); // numFiles
  appendU16BE(&listWire, static_cast<uint16_t>(rawNames.size()));
  appendU16BE(&listWire, static_cast<uint16_t>(compNames.size()));
  appendU32BE(&listWire, rfb::tight13DirSize);
  appendU32BE(&listWire, 1700000100);
  appendU32BE(&listWire, 777);
  appendU32BE(&listWire, 1700000200);
  listWire.insert(listWire.end(), compNames.begin(), compNames.end());

  conn.fileTransferClient().listDirectory("/");
  {
    ChunkedInStream in;
    in.setBuffer(listWire);
    rfb::CMsgReader r(&conn, &in);
    bool done = false;
    for (size_t i = 0; i < listWire.size() && !done; i++) {
      in.expose(1);
      done = r.readMsg();
    }
    EXPECT_TRUE(done);
  }
  ASSERT_EQ(ftHandler.listCount, 1);
  ASSERT_EQ(ftHandler.lastEntries.size(), 2U);
  EXPECT_TRUE(ftHandler.lastEntries[0].isDir);
  EXPECT_EQ(ftHandler.lastEntries[0].name, "dirA");
  EXPECT_EQ(ftHandler.lastEntries[1].size, 777U);

  // 0-byte Tight 1.3 download: FileDownloadEnd directly
  std::vector<uint8_t> zeroDl;
  appendU8(&zeroDl, rfb::msgTypeTightFileDownloadData);
  appendU8(&zeroDl, 0);
  appendU16BE(&zeroDl, 0);
  appendU16BE(&zeroDl, 0);
  appendU32BE(&zeroDl, 1700000000);
  conn.fileTransferClient().startDownload("/empty", 0);
  {
    rdr::MemInStream in(zeroDl.data(), zeroDl.size());
    rfb::CMsgReader r(&conn, &in);
    EXPECT_TRUE(r.readMsg());
  }
  EXPECT_EQ(ftHandler.downloadStartedCount, 1);
  EXPECT_TRUE(ftHandler.lastDownloadStartedOk);
  EXPECT_EQ(ftHandler.downloadCompleteCount, 1);
  EXPECT_TRUE(ftHandler.lastDownloadCompleteOk);

  // Tight 1.3 upload + Fence sync
  conn.server.supportsFence = true;
  conn.fileTransferClient().startUpload("/new.bin", true, 0);
  EXPECT_EQ(ftHandler.uploadReadyCount, 1);
  EXPECT_TRUE(ftHandler.lastUploadReadyOk);
  const uint8_t upData[5] = { 10, 20, 30, 40, 50 };
  conn.fileTransferClient().sendUploadData(upData, sizeof(upData));
  conn.fileTransferClient().finishUpload(1700000000);
  EXPECT_EQ(ftHandler.uploadCompleteCount, 0);
  conn.fileTransferClient().onFenceReply();
  EXPECT_EQ(ftHandler.uploadCompleteCount, 1);
  EXPECT_TRUE(ftHandler.lastUploadCompleteOk);
}

TEST(FileTransferUnit, UltraVNCWireRoundTrip)
{
  TestCConnection conn;
  RecordingFTHandler ftHandler;
  conn.fileTransferClient().setHandler(&ftHandler);

  rdr::MemOutStream clientOut;
  rfb::CMsgWriter clientWriter(&conn.server, &clientOut);
  conn.fileTransferClient().setWriter(&clientWriter);
  conn.fileTransferClient().onServerInit();

  // Receive UltraVNC protocol version 4
  std::vector<uint8_t> verWire;
  appendU8(&verWire, rfb::msgTypeFileTransfer);
  appendU8(&verWire, rfb::ultraFileTransferProtocolVersion);
  appendU16LE(&verWire, 4);
  appendU32BE(&verWire, 16384); // blockSize
  appendU32BE(&verWire, 0);
  {
    rdr::MemInStream in(verWire.data(), verWire.size());
    rfb::CMsgReader r(&conn, &in);
    EXPECT_TRUE(r.readMsg());
  }
  EXPECT_FALSE(conn.fileTransferClient().isSupported());

  // Server replies permission allowed (ultraFileTransferAccess size=1)
  std::vector<uint8_t> accWire;
  appendU8(&accWire, rfb::msgTypeFileTransfer);
  appendU8(&accWire, rfb::ultraFileTransferAccess);
  appendU16LE(&accWire, 0);
  appendU32BE(&accWire, 1);
  appendU32BE(&accWire, 0);
  {
    rdr::MemInStream in(accWire.data(), accWire.size());
    rfb::CMsgReader r(&conn, &in);
    EXPECT_TRUE(r.readMsg());
  }
  EXPECT_TRUE(conn.fileTransferClient().isSupported());
  EXPECT_EQ(ftHandler.lastCaps.ultraVersion, 4);
  EXPECT_EQ(ftHandler.lastCaps.ultraBlockSize, 16384U);

  // Download with > 4 GiB sizeH check and REST skip offset
  conn.fileTransferClient().startDownload("/C:/big.bin", 6);
  std::vector<uint8_t> hdrWire;
  appendU8(&hdrWire, rfb::msgTypeFileTransfer);
  appendU8(&hdrWire, rfb::ultraFileHeader);
  appendU16LE(&hdrWire, 0);
  appendU32BE(&hdrWire, 16); // low 32
  appendU32BE(&hdrWire, 0);  // length of filename (empty)
  appendU32BE(&hdrWire, 1);  // sizeH = 1 -> 4GiB + 16
  {
    rdr::MemInStream in(hdrWire.data(), hdrWire.size());
    rfb::CMsgReader r(&conn, &in);
    EXPECT_TRUE(r.readMsg());
  }
  EXPECT_EQ(ftHandler.downloadStartedCount, 1);
  EXPECT_EQ(ftHandler.lastDownloadTotal, ((1ULL << 32) | 16ULL) - 6ULL);

  // Compressed ultraFilePacket ("012345" + "Payload") -> first 6 bytes skipped
  const std::string full = "012345Payload";
  uLongf compBound = compressBound(static_cast<uLong>(full.size()));
  std::vector<uint8_t> compPkt(compBound);
  EXPECT_EQ(compress2(compPkt.data(), &compBound,
                      reinterpret_cast<const Bytef*>(full.data()),
                      static_cast<uLong>(full.size()), Z_DEFAULT_COMPRESSION),
            Z_OK);
  compPkt.resize(compBound);
  std::vector<uint8_t> pktWire;
  appendU8(&pktWire, rfb::msgTypeFileTransfer);
  appendU8(&pktWire, rfb::ultraFilePacket);
  appendU16LE(&pktWire, 0);
  appendU32BE(&pktWire, 1); // compressed = 1
  appendU32BE(&pktWire, static_cast<uint32_t>(compPkt.size()));
  pktWire.insert(pktWire.end(), compPkt.begin(), compPkt.end());
  appendU8(&pktWire, rfb::msgTypeFileTransfer);
  appendU8(&pktWire, rfb::ultraEndOfFile);
  appendU16LE(&pktWire, 0);
  appendU32BE(&pktWire, 0);
  appendU32BE(&pktWire, 0);
  {
    rdr::MemInStream in(pktWire.data(), pktWire.size());
    rfb::CMsgReader r(&conn, &in);
    EXPECT_TRUE(r.readMsg());
    EXPECT_TRUE(r.readMsg());
  }
  EXPECT_EQ(std::string(ftHandler.downloadedBytes.begin(),
                        ftHandler.downloadedBytes.end()), "Payload");
  EXPECT_TRUE(ftHandler.lastDownloadCompleteOk);

  // Verify Directory packet with LE WIN32_FIND_DATA (UTF-16LE + FILETIME)
  std::vector<uint8_t> wfd(44, 0);
  // dwFileAttributes = ultraFileAttrDir (0x10) -> LE at offset 0
  wfd[0] = rfb::ultraFileAttrDir;
  // ftLastWriteTime = Unix epoch 1700000000 -> FILETIME = (1700000000 * 10^7) + 116444736000000000
  uint64_t ftVal = 1700000000ULL * 10000000ULL + 116444736000000000ULL;
  uint32_t ftLo = static_cast<uint32_t>(ftVal & 0xFFFFFFFFU);
  uint32_t ftHi = static_cast<uint32_t>(ftVal >> 32);
  wfd[20] = static_cast<uint8_t>(ftLo & 0xFF);
  wfd[21] = static_cast<uint8_t>((ftLo >> 8) & 0xFF);
  wfd[22] = static_cast<uint8_t>((ftLo >> 16) & 0xFF);
  wfd[23] = static_cast<uint8_t>((ftLo >> 24) & 0xFF);
  wfd[24] = static_cast<uint8_t>(ftHi & 0xFF);
  wfd[25] = static_cast<uint8_t>((ftHi >> 8) & 0xFF);
  wfd[26] = static_cast<uint8_t>((ftHi >> 16) & 0xFF);
  wfd[27] = static_cast<uint8_t>((ftHi >> 24) & 0xFF);
  // UTF-16LE filename "Docs"
  const char* dname = "Docs";
  for (size_t i = 0; i < strlen(dname); i++)
    appendU16LE(&wfd, static_cast<uint16_t>(dname[i]));
  appendU16LE(&wfd, 0);

  std::vector<uint8_t> dirWire;
  // Echo packet
  appendU8(&dirWire, rfb::msgTypeFileTransfer);
  appendU8(&dirWire, rfb::ultraDirPacket);
  appendU16LE(&dirWire, rfb::ultraADirectory);
  appendU32BE(&dirWire, 0);
  appendU32BE(&dirWire, 3);
  dirWire.push_back('C'); dirWire.push_back(':'); dirWire.push_back('\\');
  // Entry packet (UTF-16LE flag 0x8000 | ultraADirectory)
  appendU8(&dirWire, rfb::msgTypeFileTransfer);
  appendU8(&dirWire, rfb::ultraDirPacket);
  appendU16LE(&dirWire, rfb::ultraADirUnicode | rfb::ultraADirectory);
  appendU32BE(&dirWire, 0);
  appendU32BE(&dirWire, static_cast<uint32_t>(wfd.size()));
  dirWire.insert(dirWire.end(), wfd.begin(), wfd.end());
  // Terminator packet (0, 0)
  appendU8(&dirWire, rfb::msgTypeFileTransfer);
  appendU8(&dirWire, rfb::ultraDirPacket);
  appendU16LE(&dirWire, 0);
  appendU32BE(&dirWire, 0);
  appendU32BE(&dirWire, 0);

  conn.fileTransferClient().listDirectory("/C:");
  {
    rdr::MemInStream in(dirWire.data(), dirWire.size());
    rfb::CMsgReader r(&conn, &in);
    EXPECT_TRUE(r.readMsg());
    EXPECT_TRUE(r.readMsg());
    EXPECT_TRUE(r.readMsg());
  }
  ASSERT_EQ(ftHandler.listCount, 1);
  ASSERT_EQ(ftHandler.lastEntries.size(), 1U);
  EXPECT_EQ(ftHandler.lastEntries[0].name, "Docs");
  EXPECT_TRUE(ftHandler.lastEntries[0].isDir);
  EXPECT_EQ(ftHandler.lastEntries[0].mtime, 1700000000LL);
}
