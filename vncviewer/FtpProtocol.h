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

#ifndef __FTPPROTOCOL_H__
#define __FTPPROTOCOL_H__

#include <stdint.h>
#include <ctime>
#include <string>

#include <rfb/fileTransferTypes.h>

namespace ftp {

  // Resolves an FTP path argument (absolute or relative to cwd) into a
  // canonical POSIX-style path starting with '/'.
  std::string normalizeFtpPath(const std::string& cwd,
                               const std::string& arg);

  // Splits a canonical FTP path into parent directory and basename.
  // For "/", parentDir is "/" and baseName is "".
  void splitFtpPath(const std::string& normalizedPath,
                    std::string* parentDir,
                    std::string* baseName);

  // Strips Telnet IAC sequences and parses "CMD[ SP arg]" into uppercase cmd
  // and verbatim arg. Returns false if the command verb is empty.
  bool stripTelnetAndParseCommand(const std::string& rawLine,
                                  std::string* cmdUpper,
                                  std::string* arg);

  // Removes leading ls-style option tokens (e.g. "-la", "-l", "-a", "--")
  // from a LIST/NLST argument and returns the remaining path expression.
  std::string stripListFlags(const std::string& arg);

  // Formats one UNIX "ls -l" directory listing line (without trailing CRLF).
  std::string formatListLine(const rfb::FileEntry& entry, time_t now);

  // Formats an RFC 3659 YYYYMMDDHHMMSS timestamp in UTC.
  std::string formatMdtm(int64_t mtime);

  // Formats one RFC 3659 MLSD entry line (without trailing CRLF).
  std::string formatMlsdLine(const rfb::FileEntry& entry);

  // Formats one RFC 3659 MLST fact line (leading space, without trailing CRLF).
  std::string formatMlstLine(const std::string& fullPath,
                             const rfb::FileEntry& entry);

  // Doubles embedded double quotes for RFC 959 257 replies.
  std::string escapeFtpPath(const std::string& path);

  // Parses PORT argument "h1,h2,h3,h4,p1,p2".
  bool parsePortArg(const std::string& arg, std::string* host, int* port);

  // Parses RFC 2428 EPRT argument "|<family>|<addr>|<port>|".
  bool parseEprtArg(const std::string& arg,
                    int* family, std::string* host, int* port);

  // Formats RFC 959 PASV 227 reply text.
  std::string formatPasvReply(const std::string& localIp, int port);

  // Formats RFC 2428 EPSV 229 reply text.
  std::string formatEpsvReply(int port);

}

#endif
