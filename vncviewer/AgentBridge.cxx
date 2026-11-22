/* Copyright 2026 TigerVNC team
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

#ifndef WIN32

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/string.h>

#include "AgentBridge.h"
#include "CConn.h"
#include "parameters.h"

static core::LogWriter vlog("AgentBridge");

namespace {

const size_t MAX_HEADER_BYTES = 1024 * 1024;
const size_t MAX_INBOUND_PAYLOAD_BYTES = 16 * 1024 * 1024;
const size_t MAX_CLIENT_QUEUE_BYTES = 128 * 1024 * 1024;
const size_t MAX_INFLIGHT_PER_CLIENT = 16;
const int MAX_JSON_DEPTH = 32;

char g_activeSocketPath[sizeof(sockaddr_un::sun_path)] = {0};
struct stat g_activeSocketStat;
volatile sig_atomic_t g_hasActiveSocketPath = 0;

uint32_t readBE32(const uint8_t* p)
{
  return ((uint32_t)p[0] << 24) |
         ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}

void writeBE32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)((v >> 24) & 0xff);
  p[1] = (uint8_t)((v >> 16) & 0xff);
  p[2] = (uint8_t)((v >> 8) & 0xff);
  p[3] = (uint8_t)(v & 0xff);
}

void setNonBlockCloexec(int fd)
{
  int flags = fcntl(fd, F_GETFD, 0);
  if (flags >= 0)
    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);

  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    throw std::runtime_error(core::format("fcntl(O_NONBLOCK) failed: %s",
                                          strerror(errno)));
}

void createPipePair(int fds[2])
{
  if (pipe(fds) != 0)
    throw std::runtime_error(core::format("pipe() failed: %s",
                                          strerror(errno)));
  try {
    setNonBlockCloexec(fds[0]);
    setNonBlockCloexec(fds[1]);
  } catch (...) {
    close(fds[0]);
    close(fds[1]);
    fds[0] = -1;
    fds[1] = -1;
    throw;
  }
}

std::string jsonEscape(const std::string& s)
{
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char ch = (unsigned char)s[i];
    switch (ch) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (ch < 0x20) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)ch);
        out += buf;
      } else {
        out.push_back((char)ch);
      }
      break;
    }
  }
  out.push_back('"');
  return out;
}

class JsonValue {
public:
  enum Type {
    T_NULL,
    T_BOOL,
    T_INT,
    T_DOUBLE,
    T_STRING,
    T_ARRAY,
    T_OBJECT
  };

  JsonValue() : type(T_NULL), boolVal(false), intVal(0), doubleVal(0.0) {}

  bool isNull() const { return type == T_NULL; }
  bool isBool() const { return type == T_BOOL; }
  bool isInt() const { return type == T_INT; }
  bool isDouble() const { return type == T_DOUBLE; }
  bool isString() const { return type == T_STRING; }
  bool isArray() const { return type == T_ARRAY; }
  bool isObject() const { return type == T_OBJECT; }

  const JsonValue* get(const char* key) const
  {
    if (type != T_OBJECT)
      return nullptr;
    std::map<std::string, JsonValue>::const_iterator it = objVal.find(key);
    if (it == objVal.end())
      return nullptr;
    return &it->second;
  }

  bool getInt(const char* key, int64_t* out) const
  {
    const JsonValue* v = get(key);
    if (!v || !v->isInt())
      return false;
    *out = v->intVal;
    return true;
  }

  bool getBool(const char* key, bool* out) const
  {
    const JsonValue* v = get(key);
    if (!v || !v->isBool())
      return false;
    *out = v->boolVal;
    return true;
  }

  bool getString(const char* key, std::string* out) const
  {
    const JsonValue* v = get(key);
    if (!v || !v->isString())
      return false;
    *out = v->strVal;
    return true;
  }

  std::string serializeScalar() const
  {
    char buf[64];
    switch (type) {
    case T_NULL:
      return "null";
    case T_BOOL:
      return boolVal ? "true" : "false";
    case T_INT:
      snprintf(buf, sizeof(buf), "%lld", (long long)intVal);
      return buf;
    case T_DOUBLE:
      snprintf(buf, sizeof(buf), "%.17g", doubleVal);
      return buf;
    case T_STRING:
      return jsonEscape(strVal);
    default:
      return "null";
    }
  }

  Type type;
  bool boolVal;
  int64_t intVal;
  double doubleVal;
  std::string strVal;
  std::vector<JsonValue> arrVal;
  std::map<std::string, JsonValue> objVal;
};

class JsonParser {
public:
  explicit JsonParser(const std::string& input)
    : data_(input.data()), len_(input.size()), pos_(0)
  {}

  bool parse(JsonValue* out)
  {
    skipWs();
    if (!parseValue(out, 0))
      return false;
    skipWs();
    return pos_ == len_;
  }

private:
  void skipWs()
  {
    while (pos_ < len_) {
      char c = data_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        pos_++;
      else
        break;
    }
  }

  bool parseValue(JsonValue* out, int depth)
  {
    if (depth > MAX_JSON_DEPTH || pos_ >= len_)
      return false;

    char c = data_[pos_];
    if (c == '{')
      return parseObject(out, depth + 1);
    if (c == '[')
      return parseArray(out, depth + 1);
    if (c == '"') {
      out->type = JsonValue::T_STRING;
      return parseRawString(&out->strVal);
    }
    if (c == 't') {
      if (len_ - pos_ >= 4 && memcmp(data_ + pos_, "true", 4) == 0) {
        pos_ += 4;
        out->type = JsonValue::T_BOOL;
        out->boolVal = true;
        return true;
      }
      return false;
    }
    if (c == 'f') {
      if (len_ - pos_ >= 5 && memcmp(data_ + pos_, "false", 5) == 0) {
        pos_ += 5;
        out->type = JsonValue::T_BOOL;
        out->boolVal = false;
        return true;
      }
      return false;
    }
    if (c == 'n') {
      if (len_ - pos_ >= 4 && memcmp(data_ + pos_, "null", 4) == 0) {
        pos_ += 4;
        out->type = JsonValue::T_NULL;
        return true;
      }
      return false;
    }
    if (c == '-' || (c >= '0' && c <= '9'))
      return parseNumber(out);

    return false;
  }

  bool parseHex4(unsigned* out)
  {
    if (len_ - pos_ < 4)
      return false;
    unsigned val = 0;
    for (int i = 0; i < 4; i++) {
      char c = data_[pos_++];
      val <<= 4;
      if (c >= '0' && c <= '9')
        val |= (unsigned)(c - '0');
      else if (c >= 'a' && c <= 'f')
        val |= (unsigned)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        val |= (unsigned)(c - 'A' + 10);
      else
        return false;
    }
    *out = val;
    return true;
  }

  bool parseRawString(std::string* out)
  {
    if (pos_ >= len_ || data_[pos_] != '"')
      return false;
    pos_++;
    out->clear();

    while (pos_ < len_) {
      unsigned char c = (unsigned char)data_[pos_++];
      if (c == '"')
        return true;
      if (c < 0x20)
        return false;
      if (c != '\\') {
        out->push_back((char)c);
        continue;
      }

      if (pos_ >= len_)
        return false;
      char esc = data_[pos_++];
      switch (esc) {
      case '"':
      case '\\':
      case '/':
        out->push_back(esc);
        break;
      case 'b':
        out->push_back('\b');
        break;
      case 'f':
        out->push_back('\f');
        break;
      case 'n':
        out->push_back('\n');
        break;
      case 'r':
        out->push_back('\r');
        break;
      case 't':
        out->push_back('\t');
        break;
      case 'u': {
        unsigned cp = 0;
        if (!parseHex4(&cp))
          return false;
        if (cp >= 0xd800 && cp <= 0xdbff) {
          if (len_ - pos_ >= 6 && data_[pos_] == '\\' && data_[pos_ + 1] == 'u') {
            pos_ += 2;
            unsigned low = 0;
            if (!parseHex4(&low) || low < 0xdc00 || low > 0xdfff)
              return false;
            cp = 0x10000 + (((cp - 0xd800) << 10) | (low - 0xdc00));
          } else {
            return false;
          }
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
          return false;
        }
        char utf8[5];
        size_t n = core::ucs4ToUTF8(cp, utf8);
        out->append(utf8, n);
        break;
      }
      default:
        return false;
      }
    }

    return false;
  }

  bool parseNumber(JsonValue* out)
  {
    size_t start = pos_;
    if (data_[pos_] == '-')
      pos_++;

    if (pos_ >= len_)
      return false;

    if (data_[pos_] == '0') {
      pos_++;
    } else if (data_[pos_] >= '1' && data_[pos_] <= '9') {
      while (pos_ < len_ && data_[pos_] >= '0' && data_[pos_] <= '9')
        pos_++;
    } else {
      return false;
    }

    bool isFloat = false;
    if (pos_ < len_ && data_[pos_] == '.') {
      isFloat = true;
      pos_++;
      if (pos_ >= len_ || data_[pos_] < '0' || data_[pos_] > '9')
        return false;
      while (pos_ < len_ && data_[pos_] >= '0' && data_[pos_] <= '9')
        pos_++;
    }

    if (pos_ < len_ && (data_[pos_] == 'e' || data_[pos_] == 'E')) {
      isFloat = true;
      pos_++;
      if (pos_ < len_ && (data_[pos_] == '+' || data_[pos_] == '-'))
        pos_++;
      if (pos_ >= len_ || data_[pos_] < '0' || data_[pos_] > '9')
        return false;
      while (pos_ < len_ && data_[pos_] >= '0' && data_[pos_] <= '9')
        pos_++;
    }

    std::string token(data_ + start, pos_ - start);
    if (!isFloat) {
      errno = 0;
      char* endPtr = nullptr;
      long long val = strtoll(token.c_str(), &endPtr, 10);
      if (errno == 0 && endPtr == token.c_str() + token.size()) {
        out->type = JsonValue::T_INT;
        out->intVal = (int64_t)val;
        return true;
      }
    }

    errno = 0;
    char* endPtr = nullptr;
    double dval = strtod(token.c_str(), &endPtr);
    if (errno == ERANGE || endPtr != token.c_str() + token.size())
      return false;
    out->type = JsonValue::T_DOUBLE;
    out->doubleVal = dval;
    return true;
  }

  bool parseArray(JsonValue* out, int depth)
  {
    out->type = JsonValue::T_ARRAY;
    out->arrVal.clear();
    pos_++; // '['
    skipWs();

    if (pos_ < len_ && data_[pos_] == ']') {
      pos_++;
      return true;
    }

    while (true) {
      JsonValue elem;
      if (!parseValue(&elem, depth))
        return false;
      out->arrVal.push_back(std::move(elem));

      skipWs();
      if (pos_ >= len_)
        return false;
      if (data_[pos_] == ']') {
        pos_++;
        return true;
      }
      if (data_[pos_] != ',')
        return false;
      pos_++;
      skipWs();
    }
  }

  bool parseObject(JsonValue* out, int depth)
  {
    out->type = JsonValue::T_OBJECT;
    out->objVal.clear();
    pos_++; // '{'
    skipWs();

    if (pos_ < len_ && data_[pos_] == '}') {
      pos_++;
      return true;
    }

    while (true) {
      std::string key;
      if (!parseRawString(&key))
        return false;
      skipWs();
      if (pos_ >= len_ || data_[pos_] != ':')
        return false;
      pos_++;
      skipWs();

      JsonValue val;
      if (!parseValue(&val, depth))
        return false;
      out->objVal[key] = std::move(val);

      skipWs();
      if (pos_ >= len_)
        return false;
      if (data_[pos_] == '}') {
        pos_++;
        return true;
      }
      if (data_[pos_] != ',')
        return false;
      pos_++;
      skipWs();
    }
  }

private:
  const char* data_;
  size_t len_;
  size_t pos_;
};

std::string buildJsonResponse(bool ok,
                              const std::string& cmd,
                              const JsonValue* idVal,
                              const std::string& extraFields)
{
  std::string s = "{";
  s += "\"ok\":";
  s += ok ? "true" : "false";
  if (!cmd.empty()) {
    s += ",\"cmd\":";
    s += jsonEscape(cmd);
  }
  if (idVal && !idVal->isNull() && !idVal->isObject() && !idVal->isArray()) {
    s += ",\"id\":";
    s += idVal->serializeScalar();
  }
  if (!extraFields.empty()) {
    s += ",";
    s += extraFields;
  }
  s += "}";
  return s;
}

std::string buildErrorExtra(const char* code, const std::string& message)
{
  std::string s = "\"error\":";
  s += jsonEscape(code);
  if (!message.empty()) {
    s += ",\"message\":";
    s += jsonEscape(message);
  }
  return s;
}

} // namespace

AgentBridge::AgentBridge(const std::string& socketPath)
  : socketPath_(socketPath),
    listenFd_(-1),
    conn_(nullptr),
    started_(false),
    stopRequested_(false)
{
  mainWakePipe_[0] = mainWakePipe_[1] = -1;
  ipcWakePipe_[0] = ipcWakePipe_[1] = -1;
}

AgentBridge::~AgentBridge()
{
  stop();
}

void AgentBridge::closePipePair(int fds[2])
{
  if (fds[0] >= 0) {
    close(fds[0]);
    fds[0] = -1;
  }
  if (fds[1] >= 0) {
    close(fds[1]);
    fds[1] = -1;
  }
}

void AgentBridge::start()
{
  if (started_)
    return;

  struct sockaddr_un addr;
  if (socketPath_.empty())
    throw std::invalid_argument("AgentSocket path must not be empty");
  if (socketPath_.size() >= sizeof(addr.sun_path))
    throw std::runtime_error(core::format(
      _("AgentSocket path \"%s\" exceeds maximum length (%u bytes)"),
      socketPath_.c_str(), (unsigned)(sizeof(addr.sun_path) - 1)));

  bool ownsSocket = false;
  try {
    createPipePair(mainWakePipe_);
    createPipePair(ipcWakePipe_);

    listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0)
      throw std::runtime_error(core::format("socket(AF_UNIX) failed: %s",
                                            strerror(errno)));
    setNonBlockCloexec(listenFd_);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socketPath_.c_str());

    mode_t savedUmask = umask(0777);
    int bindRes = bind(listenFd_, (const struct sockaddr*)&addr, sizeof(addr));
    int bindErr = errno;
    umask(savedUmask);
    if (bindRes < 0)
      throw std::runtime_error(core::format(
        "bind(\"%s\") failed: %s", socketPath_.c_str(), strerror(bindErr)));

    if (lstat(socketPath_.c_str(), &g_activeSocketStat) < 0)
      throw std::runtime_error(core::format(
        "lstat(\"%s\") failed: %s", socketPath_.c_str(), strerror(errno)));

    snprintf(g_activeSocketPath, sizeof(g_activeSocketPath), "%s",
             socketPath_.c_str());
    g_hasActiveSocketPath = 1;
    ownsSocket = true;

    if (chmod(socketPath_.c_str(), 0600) < 0)
      throw std::runtime_error(core::format(
        "chmod(\"%s\") failed: %s", socketPath_.c_str(), strerror(errno)));

    if (listen(listenFd_, 16) < 0)
      throw std::runtime_error(core::format(
        "listen(\"%s\") failed: %s", socketPath_.c_str(), strerror(errno)));

    stopRequested_ = false;
    ioThread_ = std::thread(&AgentBridge::ioLoop, this);
    started_ = true;

    vlog.info(_("Listening for agent IPC on %s"), socketPath_.c_str());
  } catch (...) {
    if (ownsSocket)
      cleanupOnSignal();
    if (listenFd_ >= 0) {
      close(listenFd_);
      listenFd_ = -1;
    }
    closePipePair(mainWakePipe_);
    closePipePair(ipcWakePipe_);
    throw;
  }
}

void AgentBridge::stop()
{
  if (started_) {
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      stopRequested_ = true;
    }
    wakeIpcThread();
    if (ioThread_.joinable())
      ioThread_.join();
    cleanupOnSignal();
    started_ = false;
  }

  if (listenFd_ >= 0) {
    close(listenFd_);
    listenFd_ = -1;
  }

  closePipePair(mainWakePipe_);
  closePipePair(ipcWakePipe_);

  std::lock_guard<std::mutex> lock(queueMutex_);
  reqQueue_.clear();
  respQueue_.clear();
}

void AgentBridge::cleanupOnSignal()
{
  struct stat st;
  if (g_hasActiveSocketPath && lstat(g_activeSocketPath, &st) == 0 &&
      st.st_dev == g_activeSocketStat.st_dev &&
      st.st_ino == g_activeSocketStat.st_ino)
    unlink(g_activeSocketPath);
  g_hasActiveSocketPath = 0;
}

void AgentBridge::setConnection(CConn* conn)
{
  conn_ = conn;
}

void AgentBridge::drainPipe(int fd)
{
  uint8_t buf[128];
  while (fd >= 0) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
      if (n < 0 && errno == EINTR)
        continue;
      break;
    }
  }
}

void AgentBridge::wakeIpcThread()
{
  if (ipcWakePipe_[1] < 0)
    return;
  uint8_t byte = 1;
  ssize_t n;
  do {
    n = write(ipcWakePipe_[1], &byte, 1);
  } while (n < 0 && errno == EINTR);
}

void AgentBridge::wakeMainThread()
{
  if (mainWakePipe_[1] < 0)
    return;
  uint8_t byte = 1;
  ssize_t n;
  do {
    n = write(mainWakePipe_[1], &byte, 1);
  } while (n < 0 && errno == EINTR);
}

void AgentBridge::enqueueResponse(uint64_t clientId,
                                  const std::string& headerJson,
                                  std::vector<uint8_t>&& payload)
{
  OutboundMessage msg;
  msg.clientId = clientId;
  msg.headerJson = headerJson;
  msg.payload = std::move(payload);

  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    respQueue_.push_back(std::move(msg));
  }
  wakeIpcThread();
}

void AgentBridge::processPendingRequests()
{
  drainPipe(mainWakePipe_[0]);

  std::deque<InboundRequest> pending;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    pending.swap(reqQueue_);
  }

  while (!pending.empty()) {
    handleRequest(pending.front());
    pending.pop_front();
  }
}

void AgentBridge::handleRequest(const InboundRequest& req)
{
  JsonValue root;
  JsonParser parser(req.headerJson);
  if (!parser.parse(&root) || !root.isObject()) {
    enqueueResponse(req.clientId,
                    buildJsonResponse(false, "", nullptr,
                                      buildErrorExtra("invalid_json",
                                                      "Malformed JSON header")),
                    std::vector<uint8_t>());
    return;
  }

  const JsonValue* idVal = root.get("id");
  std::string cmd;
  if (!root.getString("cmd", &cmd) || cmd.empty()) {
    enqueueResponse(req.clientId,
                    buildJsonResponse(false, "", idVal,
                                      buildErrorExtra("invalid_json",
                                                      "Missing string 'cmd' field")),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "PING") {
    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal, "\"pong\":true"),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "GET_STATUS") {
    bool connected = conn_ && conn_->isConnected();
    bool ready = conn_ && conn_->isSessionReady();
    bool paused = conn_ && conn_->isAgentPaused();
    int width = ready ? conn_->server.width() : 0;
    int height = ready ? conn_->server.height() : 0;
    const char* desktopName = conn_ ? conn_->server.name() : "";
    uint64_t frameSeq = conn_ ? conn_->getFrameSeq() : 0;
    uint64_t damageCount = conn_ ? conn_->getDamageCount() : 0;
    uint64_t resizeCount = conn_ ? conn_->getResizeCount() : 0;
    int64_t lastUpdateMs = conn_ ? conn_->getLastUpdateMs() : 0;
    bool updating = conn_ && conn_->isUpdating();
    bool hasClipboard = conn_ && conn_->getRemoteClipboard(nullptr);
    bool collab = (bool)agentCollaborate;
    bool overlay = conn_ ? conn_->isAgentOverlayEnabled()
                         : ((bool)agentOverlay || (bool)agentCollaborate);
    uint64_t humanActions = conn_ ? conn_->getHumanActionCount() : 0;
    int64_t lastHumanMs = conn_ ? conn_->getLastHumanInputMs() : 0;
    std::string pauseReason = conn_ ? conn_->getAgentPauseReason() : std::string();
    size_t pendingMsgs = conn_ ? conn_->getPendingUserMessageCount() : 0;

    std::string fields = core::format(
      "\"connected\":%s,\"ready\":%s,\"headless\":%s,\"view_only\":%s,"
      "\"agent_paused\":%s,\"pause_reason\":%s,"
      "\"collaborate\":%s,\"overlay\":%s,"
      "\"human_action_count\":%llu,\"last_human_input_ms\":%lld,"
      "\"pending_user_messages\":%llu,"
      "\"width\":%d,\"height\":%d,\"desktop_name\":%s,"
      "\"frame_seq\":%llu,\"damage_count\":%llu,\"resize_count\":%llu,"
      "\"last_update_ms\":%lld,\"updating\":%s,\"has_clipboard\":%s",
      connected ? "true" : "false",
      ready ? "true" : "false",
      (bool)headless ? "true" : "false",
      (bool)viewOnly ? "true" : "false",
      paused ? "true" : "false",
      jsonEscape(pauseReason).c_str(),
      collab ? "true" : "false",
      overlay ? "true" : "false",
      (unsigned long long)humanActions,
      (long long)lastHumanMs,
      (unsigned long long)pendingMsgs,
      width, height,
      jsonEscape(desktopName ? desktopName : "").c_str(),
      (unsigned long long)frameSeq,
      (unsigned long long)damageCount,
      (unsigned long long)resizeCount,
      (long long)lastUpdateMs,
      updating ? "true" : "false",
      hasClipboard ? "true" : "false");

    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal, fields),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "PAUSE_AGENT") {
    if (conn_)
      conn_->setAgentPaused(true, "ipc");
    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal, "\"agent_paused\":true"),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "RESUME_AGENT") {
    if (conn_)
      conn_->setAgentPaused(false);
    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal, "\"agent_paused\":false"),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "AGENT_OVERLAY") {
    if (!conn_) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("not_ready",
                                                        "No active connection")),
                      std::vector<uint8_t>());
      return;
    }

    bool clearFlag = false;
    if (root.get("clear") && !root.getBool("clear", &clearFlag)) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "clear must be a boolean")),
                      std::vector<uint8_t>());
      return;
    }
    if (clearFlag) {
      conn_->clearAgentOverlayState();
      enqueueResponse(req.clientId,
                      buildJsonResponse(true, cmd, idVal, ""),
                      std::vector<uint8_t>());
      return;
    }

    std::string state = "acting";
    std::string action;
    std::string intent;
    if (root.get("state") && !root.getString("state", &state)) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "state must be a string")),
                      std::vector<uint8_t>());
      return;
    }
    if (root.get("action") && !root.getString("action", &action)) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "action must be a string")),
                      std::vector<uint8_t>());
      return;
    }
    if (root.get("intent") && !root.getString("intent", &intent)) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "intent must be a string")),
                      std::vector<uint8_t>());
      return;
    }

    int64_t x = 0, y = 0, endX = 0, endY = 0;
    bool hasX = (root.get("x") != nullptr);
    bool hasY = (root.get("y") != nullptr);
    if (hasX != hasY ||
        (hasX && (!root.getInt("x", &x) || !root.getInt("y", &y)))) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "x and y must both be integers")),
                      std::vector<uint8_t>());
      return;
    }

    bool hasEndX = (root.get("end_x") != nullptr);
    bool hasEndY = (root.get("end_y") != nullptr);
    if (hasEndX != hasEndY ||
        (hasEndX && (!root.getInt("end_x", &endX) || !root.getInt("end_y", &endY)))) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "end_x and end_y must both be integers")),
                      std::vector<uint8_t>());
      return;
    }

    bool pulse = hasX && (state == "acting");
    if (root.get("pulse") && !root.getBool("pulse", &pulse)) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "pulse must be a boolean")),
                      std::vector<uint8_t>());
      return;
    }

    int64_t buttonMask = 0;
    if (root.get("button_mask") &&
        (!root.getInt("button_mask", &buttonMask) ||
         buttonMask < 0 || buttonMask > 0x1ff)) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "button_mask must be in [0, 0x1ff]")),
                      std::vector<uint8_t>());
      return;
    }

    conn_->setAgentOverlayState(state, action, intent,
                                hasX, (int)x, (int)y,
                                hasEndX, (int)endX, (int)endY,
                                pulse, (uint16_t)buttonMask);
    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal, ""),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "POLL_USER_MESSAGES") {
    if (!conn_) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("not_ready",
                                                        "No active connection")),
                      std::vector<uint8_t>());
      return;
    }

    std::vector<std::string> msgs = conn_->drainUserMessages();
    std::string arr = "[";
    for (size_t i = 0; i < msgs.size(); i++) {
      if (i > 0)
        arr += ",";
      arr += jsonEscape(msgs[i]);
    }
    arr += "]";

    std::string fields = core::format(
      "\"messages\":%s,\"human_action_count\":%llu,\"last_human_input_ms\":%lld,"
      "\"agent_paused\":%s,\"pause_reason\":%s",
      arr.c_str(),
      (unsigned long long)conn_->getHumanActionCount(),
      (long long)conn_->getLastHumanInputMs(),
      conn_->isAgentPaused() ? "true" : "false",
      jsonEscape(conn_->getAgentPauseReason()).c_str());

    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal, fields),
                    std::vector<uint8_t>());
    return;
  }

  if (!conn_ || !conn_->isSessionReady()) {
    enqueueResponse(req.clientId,
                    buildJsonResponse(false, cmd, idVal,
                                      buildErrorExtra("not_ready",
                                                      "RFB session is not ready")),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "GET_FRAME") {
    std::string fmt = "RGB24";
    const JsonValue* fmtVal = root.get("format");
    if (fmtVal) {
      if (!fmtVal->isString() ||
          (fmtVal->strVal != "RGB24" && fmtVal->strVal != "RGBA32")) {
        enqueueResponse(req.clientId,
                        buildJsonResponse(false, cmd, idVal,
                                          buildErrorExtra("invalid_args",
                                                          "format must be 'RGB24' or 'RGBA32'")),
                        std::vector<uint8_t>());
        return;
      }
      fmt = fmtVal->strVal;
    }

    const JsonValue* sinceVal = root.get("if_changed_since");
    if (sinceVal) {
      if (!sinceVal->isInt() || sinceVal->intVal < 0) {
        enqueueResponse(req.clientId,
                        buildJsonResponse(false, cmd, idVal,
                                          buildErrorExtra("invalid_args",
                                                          "if_changed_since must be a non-negative integer")),
                        std::vector<uint8_t>());
        return;
      }
      if ((uint64_t)sinceVal->intVal == conn_->getFrameSeq()) {
        std::string fields = core::format(
          "\"changed\":false,\"width\":%d,\"height\":%d,"
          "\"frame_seq\":%llu,\"damage_count\":%llu,\"resize_count\":%llu,"
          "\"last_update_ms\":%lld,\"updating\":%s,\"agent_paused\":%s",
          conn_->server.width(), conn_->server.height(),
          (unsigned long long)conn_->getFrameSeq(),
          (unsigned long long)conn_->getDamageCount(),
          (unsigned long long)conn_->getResizeCount(),
          (long long)conn_->getLastUpdateMs(),
          conn_->isUpdating() ? "true" : "false",
          conn_->isAgentPaused() ? "true" : "false");
        enqueueResponse(req.clientId,
                        buildJsonResponse(true, cmd, idVal, fields),
                        std::vector<uint8_t>());
        return;
      }
    }

    bool rgba32 = (fmt == "RGBA32");
    int w = 0, h = 0, strideBytes = 0, fbBpp = 0, fbDepth = 0;
    std::vector<uint8_t> pixels;
    try {
      if (!conn_->captureFramebuffer(rgba32, &w, &h, &strideBytes,
                                     &fbBpp, &fbDepth, &pixels)) {
        enqueueResponse(req.clientId,
                        buildJsonResponse(false, cmd, idVal,
                                          buildErrorExtra("not_ready",
                                                          "Framebuffer unavailable")),
                        std::vector<uint8_t>());
        return;
      }
    } catch (std::exception& e) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("rfb_error", e.what())),
                      std::vector<uint8_t>());
      return;
    }

    int bpp = rgba32 ? 32 : 24;
    int channels = rgba32 ? 4 : 3;
    std::string fields = core::format(
      "\"changed\":true,\"width\":%d,\"height\":%d,\"format\":%s,"
      "\"bytes_per_pixel\":%d,\"stride\":%d,\"byte_length\":%llu,"
      "\"frame_seq\":%llu,\"damage_count\":%llu,\"resize_count\":%llu,"
      "\"last_update_ms\":%lld,\"updating\":%s,\"agent_paused\":%s,"
      "\"pixel_format\":{\"bpp\":%d,\"depth\":24,\"channels\":%d,"
      "\"fb_bpp\":%d,\"fb_depth\":%d}",
      w, h, jsonEscape(fmt).c_str(),
      channels, strideBytes, (unsigned long long)pixels.size(),
      (unsigned long long)conn_->getFrameSeq(),
      (unsigned long long)conn_->getDamageCount(),
      (unsigned long long)conn_->getResizeCount(),
      (long long)conn_->getLastUpdateMs(),
      conn_->isUpdating() ? "true" : "false",
      conn_->isAgentPaused() ? "true" : "false",
      bpp, channels, fbBpp, fbDepth);

    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal, fields),
                    std::move(pixels));
    return;
  }

  if (cmd == "CLIPBOARD_GET") {
    std::string clipText;
    bool available = conn_->getRemoteClipboard(&clipText);
    std::string fields = core::format(
      "\"available\":%s,\"text\":%s",
      available ? "true" : "false",
      jsonEscape(clipText).c_str());
    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal, fields),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "REQUEST_REFRESH") {
    try {
      conn_->refreshFramebuffer();
      conn_->ensureSocketWriteWatch();
    } catch (std::exception& e) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("rfb_error", e.what())),
                      std::vector<uint8_t>());
      return;
    }
    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal, ""),
                    std::vector<uint8_t>());
    return;
  }

  if (viewOnly) {
    enqueueResponse(req.clientId,
                    buildJsonResponse(false, cmd, idVal,
                                      buildErrorExtra("view_only",
                                                      "Viewer is running in ViewOnly mode")),
                    std::vector<uint8_t>());
    return;
  }

  if (conn_->isAgentPaused()) {
    enqueueResponse(req.clientId,
                    buildJsonResponse(false, cmd, idVal,
                                      buildErrorExtra("agent_paused",
                                                      "Agent input is paused by operator")),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "POINTER_EVENT") {
    int64_t x = 0, y = 0, buttonMask = 0;
    if (!root.getInt("x", &x) || !root.getInt("y", &y) ||
        !root.getInt("button_mask", &buttonMask) ||
        x < std::numeric_limits<int>::min() ||
        x > std::numeric_limits<int>::max() ||
        y < std::numeric_limits<int>::min() ||
        y > std::numeric_limits<int>::max() ||
        buttonMask < 0 || buttonMask > 0x1ff) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "Expected integer x, y and button_mask in [0, 0x1ff]")),
                      std::vector<uint8_t>());
      return;
    }

    try {
      conn_->injectPointerEvent((int)x, (int)y, (uint16_t)buttonMask);
    } catch (std::exception& e) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("rfb_error", e.what())),
                      std::vector<uint8_t>());
      return;
    }
    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal, ""),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "KEY_EVENT") {
    int64_t keysym = 0;
    int64_t keycode = 0;
    bool down = false;
    if (!root.getInt("keysym", &keysym) || !root.getBool("down", &down) ||
        keysym < 0 || keysym > 0xffffffffLL) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "Expected uint32 keysym and bool down")),
                      std::vector<uint8_t>());
      return;
    }
    const JsonValue* kcVal = root.get("keycode");
    if (kcVal) {
      if (!kcVal->isInt() || kcVal->intVal < 0 || kcVal->intVal > 0xffffffffLL) {
        enqueueResponse(req.clientId,
                        buildJsonResponse(false, cmd, idVal,
                                          buildErrorExtra("invalid_args",
                                                          "Expected uint32 keycode")),
                        std::vector<uint8_t>());
        return;
      }
      keycode = kcVal->intVal;
    }
    if (keysym == 0 && keycode == 0) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "keysym and keycode cannot both be 0")),
                      std::vector<uint8_t>());
      return;
    }

    try {
      conn_->injectKeyEvent((uint32_t)keysym, (uint32_t)keycode, down);
    } catch (std::exception& e) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("rfb_error", e.what())),
                      std::vector<uint8_t>());
      return;
    }
    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal, ""),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "TEXT_TYPE") {
    std::string text;
    if (!root.getString("text", &text) ||
        (text.find('\0') != std::string::npos) ||
        !core::isValidUTF8(text.c_str(), text.size())) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "Expected valid UTF-8 string 'text'")),
                      std::vector<uint8_t>());
      return;
    }

    size_t charsSent = 0;
    try {
      charsSent = conn_->injectText(text);
    } catch (std::exception& e) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("rfb_error", e.what())),
                      std::vector<uint8_t>());
      return;
    }

    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal,
                                      core::format("\"chars_sent\":%llu",
                                                   (unsigned long long)charsSent)),
                    std::vector<uint8_t>());
    return;
  }

  if (cmd == "CLIPBOARD_SET") {
    std::string text;
    const JsonValue* textVal = root.get("text");
    if (textVal) {
      if (!textVal->isString()) {
        enqueueResponse(req.clientId,
                        buildJsonResponse(false, cmd, idVal,
                                          buildErrorExtra("invalid_args",
                                                          "Expected string 'text'")),
                        std::vector<uint8_t>());
        return;
      }
      text = textVal->strVal;
    } else {
      text.assign((const char*)req.payload.data(), req.payload.size());
    }

    if ((text.find('\0') != std::string::npos) ||
        !core::isValidUTF8(text.c_str(), text.size())) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("invalid_args",
                                                        "Clipboard payload must be valid UTF-8 without NUL bytes")),
                      std::vector<uint8_t>());
      return;
    }

    try {
      conn_->injectClipboard(text);
    } catch (std::exception& e) {
      enqueueResponse(req.clientId,
                      buildJsonResponse(false, cmd, idVal,
                                        buildErrorExtra("rfb_error", e.what())),
                      std::vector<uint8_t>());
      return;
    }

    enqueueResponse(req.clientId,
                    buildJsonResponse(true, cmd, idVal,
                                      core::format("\"bytes\":%llu",
                                                   (unsigned long long)text.size())),
                    std::vector<uint8_t>());
    return;
  }

  enqueueResponse(req.clientId,
                  buildJsonResponse(false, cmd, idVal,
                                    buildErrorExtra("unknown_cmd",
                                                    core::format("Unknown command '%s'",
                                                                 cmd.c_str()))),
                  std::vector<uint8_t>());
}

void AgentBridge::ioLoop()
{
  std::map<uint64_t, ClientState> clients;
  uint64_t nextClientId = 1;

  while (true) {
    bool stopping = false;
    std::deque<OutboundMessage> pendingOutbound;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      stopping = stopRequested_;
      pendingOutbound.swap(respQueue_);
    }

    if (stopping)
      break;

    while (!pendingOutbound.empty()) {
      OutboundMessage& out = pendingOutbound.front();
      std::map<uint64_t, ClientState>::iterator it = clients.find(out.clientId);
      if (it != clients.end()) {
        ClientState& client = it->second;
        if (client.pendingRequests > 0)
          client.pendingRequests--;

        size_t totalFrameBytes = 8 + out.headerJson.size() + out.payload.size();
        if (client.queuedBytes + totalFrameBytes > MAX_CLIENT_QUEUE_BYTES) {
          vlog.error("Closing slow agent client %llu (write queue exceeded limit)",
                     (unsigned long long)client.id);
          close(client.fd);
          clients.erase(it);
        } else {
          std::vector<uint8_t> frame(totalFrameBytes);
          writeBE32(frame.data(), (uint32_t)out.headerJson.size());
          writeBE32(frame.data() + 4, (uint32_t)out.payload.size());
          if (!out.headerJson.empty())
            memcpy(frame.data() + 8, out.headerJson.data(), out.headerJson.size());
          if (!out.payload.empty())
            memcpy(frame.data() + 8 + out.headerJson.size(),
                   out.payload.data(), out.payload.size());
          client.queuedBytes += frame.size();
          client.writeQueue.push_back(std::move(frame));
        }
      }
      pendingOutbound.pop_front();
    }

    std::vector<struct pollfd> fds;
    std::vector<uint64_t> clientIds;
    fds.reserve(2 + clients.size());
    clientIds.reserve(clients.size());

    struct pollfd pfd;
    pfd.fd = ipcWakePipe_[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    fds.push_back(pfd);

    pfd.fd = listenFd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    fds.push_back(pfd);

    for (std::map<uint64_t, ClientState>::const_iterator it = clients.begin();
         it != clients.end(); ++it) {
      pfd.fd = it->second.fd;
      pfd.events = 0;
      if (it->second.pendingRequests < MAX_INFLIGHT_PER_CLIENT)
        pfd.events |= POLLIN;
      if (!it->second.writeQueue.empty())
        pfd.events |= POLLOUT;
      pfd.revents = 0;
      fds.push_back(pfd);
      clientIds.push_back(it->first);
    }

    int ret = poll(fds.data(), (nfds_t)fds.size(), -1);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      vlog.error("poll() failed in AgentBridge: %s", strerror(errno));
      break;
    }

    if (fds[0].revents & POLLIN)
      drainPipe(ipcWakePipe_[0]);

    if (fds[1].revents & POLLIN) {
      while (true) {
        int cfd = accept(listenFd_, nullptr, nullptr);
        if (cfd < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          if (errno == EINTR)
            continue;
          vlog.error("accept() failed in AgentBridge: %s", strerror(errno));
          break;
        }
        try {
          setNonBlockCloexec(cfd);
#ifdef SO_NOSIGPIPE
          int one = 1;
          setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
          ClientState cs;
          cs.id = nextClientId++;
          cs.fd = cfd;
          cs.writeOffset = 0;
          cs.queuedBytes = 0;
          cs.pendingRequests = 0;
          clients[cs.id] = std::move(cs);
        } catch (std::exception& e) {
          vlog.error("Failed configuring agent client socket: %s", e.what());
          close(cfd);
        }
      }
    }

    bool pushedRequests = false;
    for (size_t idx = 0; idx < clientIds.size(); idx++) {
      short revents = fds[2 + idx].revents;
      if (revents == 0)
        continue;

      std::map<uint64_t, ClientState>::iterator it = clients.find(clientIds[idx]);
      if (it == clients.end())
        continue;

      ClientState& client = it->second;
      bool dropClient = false;

      if (revents & POLLIN) {
        uint8_t buf[65536];
        while (true) {
          ssize_t n = recv(client.fd, buf, sizeof(buf), 0);
          if (n > 0) {
            client.readBuf.insert(client.readBuf.end(), buf, buf + (size_t)n);
          } else if (n == 0) {
            dropClient = true;
            break;
          } else {
            if (errno == EINTR)
              continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              break;
            dropClient = true;
            break;
          }
        }

        size_t consumed = 0;
        while (!dropClient && (client.readBuf.size() - consumed) >= 8) {
          const uint8_t* ptr = client.readBuf.data() + consumed;
          uint32_t headerLen = readBE32(ptr);
          uint32_t payloadLen = readBE32(ptr + 4);

          if ((headerLen == 0) ||
              (headerLen > MAX_HEADER_BYTES) ||
              (payloadLen > MAX_INBOUND_PAYLOAD_BYTES)) {
            vlog.error("Dropping agent client %llu due to invalid frame lengths (%u, %u)",
                       (unsigned long long)client.id,
                       (unsigned)headerLen, (unsigned)payloadLen);
            dropClient = true;
            break;
          }

          size_t frameSize = 8 + (size_t)headerLen + (size_t)payloadLen;
          if ((client.readBuf.size() - consumed) < frameSize)
            break;

          InboundRequest req;
          req.clientId = client.id;
          req.headerJson.assign((const char*)(ptr + 8), headerLen);
          if (payloadLen > 0) {
            req.payload.assign(ptr + 8 + headerLen,
                               ptr + 8 + headerLen + payloadLen);
          }
          consumed += frameSize;
          client.pendingRequests++;

          {
            std::lock_guard<std::mutex> lock(queueMutex_);
            reqQueue_.push_back(std::move(req));
          }
          pushedRequests = true;
        }

        if (consumed > 0 && !dropClient)
          client.readBuf.erase(client.readBuf.begin(),
                               client.readBuf.begin() + (ptrdiff_t)consumed);
      }

      if (!dropClient && (revents & POLLOUT)) {
        while (!client.writeQueue.empty()) {
          const std::vector<uint8_t>& front = client.writeQueue.front();
          size_t rem = front.size() - client.writeOffset;
#ifdef MSG_NOSIGNAL
          ssize_t n = send(client.fd, front.data() + client.writeOffset,
                           rem, MSG_NOSIGNAL);
#else
          ssize_t n = send(client.fd, front.data() + client.writeOffset,
                           rem, 0);
#endif
          if (n > 0) {
            client.writeOffset += (size_t)n;
            client.queuedBytes -= (size_t)n;
            if (client.writeOffset >= front.size()) {
              client.writeQueue.pop_front();
              client.writeOffset = 0;
            }
          } else {
            if (n < 0 && errno == EINTR)
              continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
              break;
            dropClient = true;
            break;
          }
        }
      }

      if (!dropClient && (revents & (POLLERR | POLLHUP | POLLNVAL))) {
        if (revents & (POLLERR | POLLNVAL))
          dropClient = true;
        else if ((revents & POLLHUP) && !(revents & POLLIN))
          dropClient = true;
      }

      if (dropClient) {
        close(client.fd);
        clients.erase(it);
      }
    }

    if (pushedRequests)
      wakeMainThread();
  }

  for (std::map<uint64_t, ClientState>::iterator it = clients.begin();
       it != clients.end(); ++it) {
    close(it->second.fd);
  }
  clients.clear();
}

#endif /* !WIN32 */
