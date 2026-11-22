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

#ifndef __RFB_TIGHTCAPABILITY_H__
#define __RFB_TIGHTCAPABILITY_H__

#include <stdint.h>
#include <string.h>

namespace rfb {

  struct TightCapability {
    int32_t code;
    char vendor[5];
    char signature[9];

    TightCapability() : code(0) {
      memset(vendor, 0, sizeof(vendor));
      memset(signature, 0, sizeof(signature));
    }

    bool matches(int32_t c, const char* v, const char* s) const {
      return code == c &&
             strncmp(vendor, v, 4) == 0 &&
             strncmp(signature, s, 8) == 0;
    }
  };

  // Vendor signatures
  static const char tightVendorStandard[] = "STDV";
  static const char tightVendorTight[]    = "TGHT";
  static const char tightVendorVeNCrypt[] = "VENC";
  static const char tightVendorTridia[]   = "TRDV";

  // Tunnel capabilities
  static const int32_t tightTunnelNone    = 0;
  static const char sigTightTunnelNone[]  = "NOTUNNEL";

  // Authentication capabilities
  static const int32_t tightAuthNone      = 1;
  static const char sigTightAuthNone[]    = "NOAUTH__";

  static const int32_t tightAuthVnc       = 2;
  static const char sigTightAuthVnc[]     = "VNCAUTH_";

  static const int32_t tightAuthVeNCrypt  = 19;
  static const char sigTightAuthVeNCrypt[] = "VENCRYPT";

  static const int32_t tightAuthUnixLogin = 129;
  static const char sigTightAuthUnixLogin[] = "ULGNAUTH";

  static const int32_t tightAuthExternal  = 130;
  static const char sigTightAuthExternal[] = "XTRNAUTH";

}

#endif
