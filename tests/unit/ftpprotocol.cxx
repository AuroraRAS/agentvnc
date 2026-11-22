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

#include <gtest/gtest.h>

#include "../../vncviewer/FtpProtocol.h"

TEST(FtpProtocol, NormalizeFtpPath)
{
  EXPECT_EQ(ftp::normalizeFtpPath("/", ""), "/");
  EXPECT_EQ(ftp::normalizeFtpPath("/a/b", ""), "/a/b");
  EXPECT_EQ(ftp::normalizeFtpPath("/a/b", "."), "/a/b");
  EXPECT_EQ(ftp::normalizeFtpPath("/a/b", ".."), "/a");
  EXPECT_EQ(ftp::normalizeFtpPath("/a/b", "../.."), "/");
  EXPECT_EQ(ftp::normalizeFtpPath("/a/b", "../../.."), "/");
  EXPECT_EQ(ftp::normalizeFtpPath("/a/b", "c//d/./e/../f/"), "/a/b/c/d/f");
  EXPECT_EQ(ftp::normalizeFtpPath("/a/b", "/x//y/../z/"), "/x/z");
  EXPECT_EQ(ftp::normalizeFtpPath("/a/b", "/"), "/");
}

TEST(FtpProtocol, SplitFtpPath)
{
  std::string parent, base;

  ftp::splitFtpPath("/", &parent, &base);
  EXPECT_EQ(parent, "/");
  EXPECT_EQ(base, "");

  ftp::splitFtpPath("/hello.txt", &parent, &base);
  EXPECT_EQ(parent, "/");
  EXPECT_EQ(base, "hello.txt");

  ftp::splitFtpPath("/dir/sub/file with spaces.bin", &parent, &base);
  EXPECT_EQ(parent, "/dir/sub");
  EXPECT_EQ(base, "file with spaces.bin");
}

TEST(FtpProtocol, StripTelnetAndParseCommand)
{
  std::string cmd, arg;

  EXPECT_TRUE(ftp::stripTelnetAndParseCommand("user alice\r\n", &cmd, &arg));
  EXPECT_EQ(cmd, "USER");
  EXPECT_EQ(arg, "alice");

  EXPECT_TRUE(ftp::stripTelnetAndParseCommand("  pwd\r\n", &cmd, &arg));
  EXPECT_EQ(cmd, "PWD");
  EXPECT_EQ(arg, "");

  // Telnet IAC IP + IAC DM before ABOR
  std::string abor("\xFF\xF4\xFF\xF2""abor\r\n", 10);
  EXPECT_TRUE(ftp::stripTelnetAndParseCommand(abor, &cmd, &arg));
  EXPECT_EQ(cmd, "ABOR");
  EXPECT_EQ(arg, "");

  // Telnet 3-byte DO option + filename with leading space preserved
  std::string retr("\xFF\xFD\x03retr  leading.txt\r\n");
  EXPECT_TRUE(ftp::stripTelnetAndParseCommand(retr, &cmd, &arg));
  EXPECT_EQ(cmd, "RETR");
  EXPECT_EQ(arg, " leading.txt");

  // Escaped 0xFF 0xFF inside argument
  std::string cwd("CWD /\xFF\xFF""dir\r\n");
  EXPECT_TRUE(ftp::stripTelnetAndParseCommand(cwd, &cmd, &arg));
  EXPECT_EQ(cmd, "CWD");
  EXPECT_EQ(arg, "/\xFF""dir");

  EXPECT_FALSE(ftp::stripTelnetAndParseCommand("\r\n", &cmd, &arg));
}

TEST(FtpProtocol, StripListFlags)
{
  EXPECT_EQ(ftp::stripListFlags(""), "");
  EXPECT_EQ(ftp::stripListFlags("-la"), "");
  EXPECT_EQ(ftp::stripListFlags("-l -a"), "");
  EXPECT_EQ(ftp::stripListFlags("-la /some/dir name"), "/some/dir name");
  EXPECT_EQ(ftp::stripListFlags("-- -literal-name"), "-literal-name");
  EXPECT_EQ(ftp::stripListFlags("/dir/sub"), "/dir/sub");
}

TEST(FtpProtocol, FormattingHelpers)
{
  rfb::FileEntry dirEntry;
  dirEntry.name = "subdir";
  dirEntry.isDir = true;
  dirEntry.mtime = 1700000000LL; // 2023-11-14 22:13:20 UTC

  rfb::FileEntry fileEntry;
  fileEntry.name = "run.sh";
  fileEntry.size = 12345;
  fileEntry.isDir = false;
  fileEntry.isExecutable = true;
  fileEntry.mtime = 1700000000LL;

  EXPECT_EQ(ftp::formatMdtm(1700000000LL), "20231114221320");
  EXPECT_EQ(ftp::formatMlsdLine(dirEntry),
            "type=dir;modify=20231114221320; subdir");
  EXPECT_EQ(ftp::formatMlsdLine(fileEntry),
            "type=file;size=12345;modify=20231114221320; run.sh");
  EXPECT_EQ(ftp::formatMlstLine("/bin/run.sh", fileEntry),
            " type=file;size=12345;modify=20231114221320; /bin/run.sh");

  // Fixed recent "now" uses HH:MM
  std::string recent = ftp::formatListLine(fileEntry, 1700000100LL);
  EXPECT_NE(recent.find("-rwxr-xr-x"), std::string::npos);
  EXPECT_NE(recent.find("12345"), std::string::npos);
  EXPECT_NE(recent.find("Nov 14 22:13 run.sh"), std::string::npos);

  // Far later "now" (> 6 months) uses YYYY
  std::string older = ftp::formatListLine(dirEntry, 1735000000LL);
  EXPECT_NE(older.find("drwxr-xr-x"), std::string::npos);
  EXPECT_NE(older.find("Nov 14  2023 subdir"), std::string::npos);

  EXPECT_EQ(ftp::escapeFtpPath("/a\"b/c"), "/a\"\"b/c");
}

TEST(FtpProtocol, AddressHelpers)
{
  std::string host;
  int port = 0;
  int family = 0;

  EXPECT_TRUE(ftp::parsePortArg("127,0,0,1,19,136", &host, &port));
  EXPECT_EQ(host, "127.0.0.1");
  EXPECT_EQ(port, 5000);
  EXPECT_FALSE(ftp::parsePortArg("127,0,0,256,19,136", &host, &port));
  EXPECT_FALSE(ftp::parsePortArg("127,0,0,1,0,0", &host, &port));

  EXPECT_TRUE(ftp::parseEprtArg("|1|127.0.0.1|5000|", &family, &host, &port));
  EXPECT_EQ(family, 1);
  EXPECT_EQ(host, "127.0.0.1");
  EXPECT_EQ(port, 5000);

  EXPECT_TRUE(ftp::parseEprtArg("|2|::1|60000|", &family, &host, &port));
  EXPECT_EQ(family, 2);
  EXPECT_EQ(host, "::1");
  EXPECT_EQ(port, 60000);

  EXPECT_FALSE(ftp::parseEprtArg("|3|127.0.0.1|5000|", &family, &host, &port));
  EXPECT_FALSE(ftp::parseEprtArg("|1|127.0.0.1|0|", &family, &host, &port));

  EXPECT_EQ(ftp::formatPasvReply("127.0.0.1", 5000),
            "227 Entering Passive Mode (127,0,0,1,19,136)");
  EXPECT_EQ(ftp::formatPasvReply("::ffff:10.0.0.5", 2121),
            "227 Entering Passive Mode (10,0,0,5,8,73)");
  EXPECT_EQ(ftp::formatEpsvReply(5000),
            "229 Entering Extended Passive Mode (|||5000|)");
}
