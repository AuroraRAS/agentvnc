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

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <core/string.h>

#include "FtpProtocol.h"

namespace ftp {

static bool toUtcTm(time_t t, struct tm* out)
{
#ifdef WIN32
  return gmtime_s(out, &t) == 0;
#else
  return gmtime_r(&t, out) != nullptr;
#endif
}

static bool parseUintRange(const std::string& s, unsigned maxVal, unsigned* out)
{
  if (s.empty() || s.size() > 5)
    return false;
  unsigned val = 0;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char ch = static_cast<unsigned char>(s[i]);
    if (ch < '0' || ch > '9')
      return false;
    val = val * 10 + static_cast<unsigned>(ch - '0');
    if (val > maxVal)
      return false;
  }
  *out = val;
  return true;
}

std::string normalizeFtpPath(const std::string& cwd, const std::string& arg)
{
  std::vector<std::string> parts;
  std::string source;

  if (!arg.empty() && arg[0] == '/') {
    source = arg;
  } else {
    if (!cwd.empty() && cwd[0] == '/')
      source = cwd;
    else
      source = "/";
    if (!arg.empty()) {
      if (source.back() != '/')
        source.push_back('/');
      source += arg;
    }
  }

  size_t i = 0;
  while (i < source.size()) {
    while (i < source.size() && source[i] == '/')
      i++;
    if (i >= source.size())
      break;
    size_t start = i;
    while (i < source.size() && source[i] != '/')
      i++;
    std::string seg = source.substr(start, i - start);
    if (seg == ".")
      continue;
    if (seg == "..") {
      if (!parts.empty())
        parts.pop_back();
      continue;
    }
    parts.push_back(seg);
  }

  if (parts.empty())
    return "/";

  std::string result;
  for (size_t j = 0; j < parts.size(); j++) {
    result.push_back('/');
    result += parts[j];
  }
  return result;
}

void splitFtpPath(const std::string& normalizedPath,
                  std::string* parentDir,
                  std::string* baseName)
{
  std::string norm = normalizeFtpPath("/", normalizedPath);
  if (norm == "/") {
    if (parentDir != nullptr)
      *parentDir = "/";
    if (baseName != nullptr)
      baseName->clear();
    return;
  }

  size_t pos = norm.rfind('/');
  if (pos == 0 || pos == std::string::npos) {
    if (parentDir != nullptr)
      *parentDir = "/";
    if (baseName != nullptr)
      *baseName = (pos == 0) ? norm.substr(1) : norm;
    return;
  }

  if (parentDir != nullptr)
    *parentDir = norm.substr(0, pos);
  if (baseName != nullptr)
    *baseName = norm.substr(pos + 1);
}

bool stripTelnetAndParseCommand(const std::string& rawLine,
                                std::string* cmdUpper,
                                std::string* arg)
{
  std::string cleaned;
  cleaned.reserve(rawLine.size());

  for (size_t i = 0; i < rawLine.size(); i++) {
    unsigned char ch = static_cast<unsigned char>(rawLine[i]);
    if (ch == 0xFF) { // Telnet IAC
      if (i + 1 >= rawLine.size())
        break;
      unsigned char code = static_cast<unsigned char>(rawLine[i + 1]);
      if (code == 0xFF) {
        cleaned.push_back(static_cast<char>(0xFF));
        i += 1;
      } else if (code >= 251 && code <= 254) {
        // 3-byte option negotiation: IAC WILL/WONT/DO/DONT <opt>
        i += (i + 2 < rawLine.size()) ? 2 : 1;
      } else {
        // 2-byte command (e.g. IAC IP, IAC DM, IAC NOP)
        i += 1;
      }
      continue;
    }
    if (ch == '\r' || ch == '\n')
      continue;
    cleaned.push_back(static_cast<char>(ch));
  }

  // Trim leading spaces/tabs before the command verb if any.
  size_t cmdStart = 0;
  while (cmdStart < cleaned.size() &&
         (cleaned[cmdStart] == ' ' || cleaned[cmdStart] == '\t'))
    cmdStart++;
  if (cmdStart >= cleaned.size()) {
    if (cmdUpper != nullptr)
      cmdUpper->clear();
    if (arg != nullptr)
      arg->clear();
    return false;
  }

  size_t sp = cleaned.find(' ', cmdStart);
  std::string verb;
  std::string rest;
  if (sp == std::string::npos) {
    verb = cleaned.substr(cmdStart);
  } else {
    verb = cleaned.substr(cmdStart, sp - cmdStart);
    rest = cleaned.substr(sp + 1);
  }

  for (size_t i = 0; i < verb.size(); i++) {
    unsigned char ch = static_cast<unsigned char>(verb[i]);
    if (ch >= 'a' && ch <= 'z')
      verb[i] = static_cast<char>(ch - 'a' + 'A');
  }

  if (cmdUpper != nullptr)
    *cmdUpper = verb;
  if (arg != nullptr)
    *arg = rest;
  return !verb.empty();
}

std::string stripListFlags(const std::string& arg)
{
  size_t pos = 0;
  while (pos < arg.size() && arg[pos] == ' ')
    pos++;

  while (pos < arg.size() && arg[pos] == '-') {
    size_t end = arg.find(' ', pos);
    std::string tok = (end == std::string::npos)
      ? arg.substr(pos)
      : arg.substr(pos, end - pos);

    if (tok == "--") {
      if (end == std::string::npos)
        return "";
      pos = end + 1;
      while (pos < arg.size() && arg[pos] == ' ')
        pos++;
      return arg.substr(pos);
    }

    if (tok.size() < 2)
      break;
    bool isFlag = true;
    for (size_t i = 1; i < tok.size(); i++) {
      unsigned char ch = static_cast<unsigned char>(tok[i]);
      if (!std::isalnum(ch)) {
        isFlag = false;
        break;
      }
    }
    if (!isFlag)
      break;

    if (end == std::string::npos)
      return "";
    pos = end + 1;
    while (pos < arg.size() && arg[pos] == ' ')
      pos++;
  }

  return arg.substr(pos);
}

std::string formatListLine(const rfb::FileEntry& entry, time_t now)
{
  static const char* kMonths[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };

  const char* perms;
  if (entry.isDir)
    perms = "drwxr-xr-x";
  else if (entry.isExecutable)
    perms = "-rwxr-xr-x";
  else
    perms = "-rw-r--r--";

  time_t ts = (entry.mtime > 0) ? static_cast<time_t>(entry.mtime) : 0;
  struct tm tmUtc;
  if (!toUtcTm(ts, &tmUtc)) {
    std::memset(&tmUtc, 0, sizeof(tmUtc));
    tmUtc.tm_mday = 1;
    tmUtc.tm_year = 70;
  }

  const char* mon = (tmUtc.tm_mon >= 0 && tmUtc.tm_mon < 12)
    ? kMonths[tmUtc.tm_mon] : "Jan";

  bool showTime = false;
  if (entry.mtime > 0 && now > 0) {
    int64_t diff = static_cast<int64_t>(now) - static_cast<int64_t>(ts);
    // Use HH:MM for timestamps within ~6 months and not far in the future.
    if (diff >= -3600 && diff <= 15552000)
      showTime = true;
  }

  char dateBuf[32];
  if (showTime) {
    std::snprintf(dateBuf, sizeof(dateBuf), "%s %2d %02d:%02d",
                  mon, tmUtc.tm_mday, tmUtc.tm_hour, tmUtc.tm_min);
  } else {
    std::snprintf(dateBuf, sizeof(dateBuf), "%s %2d  %04d",
                  mon, tmUtc.tm_mday, tmUtc.tm_year + 1900);
  }

  unsigned long long displaySize = entry.isDir
    ? 0ULL
    : static_cast<unsigned long long>(entry.size);

  return core::format("%s %3d vnc vnc %12llu %s %s",
                      perms, entry.isDir ? 2 : 1,
                      displaySize, dateBuf, entry.name.c_str());
}

std::string formatMdtm(int64_t mtime)
{
  time_t ts = (mtime > 0) ? static_cast<time_t>(mtime) : 0;
  struct tm tmUtc;
  if (!toUtcTm(ts, &tmUtc))
    return "19700101000000";

  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d",
                tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
                tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);
  return std::string(buf);
}

std::string formatMlsdLine(const rfb::FileEntry& entry)
{
  std::string out;
  if (entry.isDir) {
    out = "type=dir;";
  } else {
    out = core::format("type=file;size=%llu;",
                       static_cast<unsigned long long>(entry.size));
  }
  if (entry.mtime > 0) {
    out += "modify=";
    out += formatMdtm(entry.mtime);
    out += ";";
  }
  out.push_back(' ');
  out += entry.name;
  return out;
}

std::string formatMlstLine(const std::string& fullPath,
                           const rfb::FileEntry& entry)
{
  std::string out = " ";
  if (entry.isDir) {
    out += "type=dir;";
  } else {
    out += core::format("type=file;size=%llu;",
                        static_cast<unsigned long long>(entry.size));
  }
  if (entry.mtime > 0) {
    out += "modify=";
    out += formatMdtm(entry.mtime);
    out += ";";
  }
  out.push_back(' ');
  out += fullPath;
  return out;
}

std::string escapeFtpPath(const std::string& path)
{
  std::string out;
  out.reserve(path.size());
  for (size_t i = 0; i < path.size(); i++) {
    if (path[i] == '"')
      out += "\"\"";
    else
      out.push_back(path[i]);
  }
  return out;
}

bool parsePortArg(const std::string& arg, std::string* host, int* port)
{
  std::string trimmed = arg;
  while (!trimmed.empty() && trimmed.back() == ' ')
    trimmed.pop_back();

  std::vector<std::string> parts = core::split(trimmed.c_str(), ',');
  if (parts.size() != 6)
    return false;

  unsigned vals[6];
  for (size_t i = 0; i < 6; i++) {
    if (!parseUintRange(parts[i], 255, &vals[i]))
      return false;
  }

  unsigned p = (vals[4] << 8) | vals[5];
  if (p == 0 || p > 65535)
    return false;

  if (host != nullptr) {
    *host = core::format("%u.%u.%u.%u", vals[0], vals[1], vals[2], vals[3]);
  }
  if (port != nullptr)
    *port = static_cast<int>(p);
  return true;
}

bool parseEprtArg(const std::string& arg,
                  int* family, std::string* host, int* port)
{
  if (arg.size() < 7)
    return false;

  unsigned char delim = static_cast<unsigned char>(arg[0]);
  if (delim < 33 || delim > 126)
    return false;
  if (static_cast<unsigned char>(arg.back()) != delim)
    return false;

  std::string body = arg.substr(1, arg.size() - 2);
  size_t p1 = body.find(static_cast<char>(delim));
  if (p1 == std::string::npos)
    return false;
  size_t p2 = body.find(static_cast<char>(delim), p1 + 1);
  if (p2 == std::string::npos)
    return false;
  if (body.find(static_cast<char>(delim), p2 + 1) != std::string::npos)
    return false;

  std::string famStr = body.substr(0, p1);
  std::string addrStr = body.substr(p1 + 1, p2 - p1 - 1);
  std::string portStr = body.substr(p2 + 1);

  unsigned fam = 0;
  if (!parseUintRange(famStr, 2, &fam) || (fam != 1 && fam != 2))
    return false;
  if (addrStr.empty())
    return false;
  unsigned p = 0;
  if (!parseUintRange(portStr, 65535, &p) || p == 0)
    return false;

  if (family != nullptr)
    *family = static_cast<int>(fam);
  if (host != nullptr)
    *host = addrStr;
  if (port != nullptr)
    *port = static_cast<int>(p);
  return true;
}

std::string formatPasvReply(const std::string& localIp, int port)
{
  std::string ip = localIp;
  const std::string v4Mapped = "::ffff:";
  if (ip.size() > v4Mapped.size() &&
      ip.compare(0, v4Mapped.size(), v4Mapped) == 0) {
    ip = ip.substr(v4Mapped.size());
  }

  std::vector<std::string> octets = core::split(ip.c_str(), '.');
  unsigned v[4] = {127, 0, 0, 1};
  bool validV4 = (octets.size() == 4);
  if (validV4) {
    for (size_t i = 0; i < 4; i++) {
      if (!parseUintRange(octets[i], 255, &v[i])) {
        validV4 = false;
        break;
      }
    }
  }
  if (!validV4) {
    v[0] = 127;
    v[1] = 0;
    v[2] = 0;
    v[3] = 1;
  }

  unsigned p1 = (static_cast<unsigned>(port) >> 8) & 0xFF;
  unsigned p2 = static_cast<unsigned>(port) & 0xFF;
  return core::format("227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
                      v[0], v[1], v[2], v[3], p1, p2);
}

std::string formatEpsvReply(int port)
{
  return core::format("229 Entering Extended Passive Mode (|||%d|)", port);
}

}
