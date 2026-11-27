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

#ifndef __RFB_CSECURITYTIGHT_H__
#define __RFB_CSECURITYTIGHT_H__

#include <stdint.h>

#include <rfb/CSecurity.h>
#include <rfb/Security.h>

namespace rfb {

  class SecurityClient;

  class CSecurityTight : public CSecurity {
  public:
    CSecurityTight(CConnection* cc, SecurityClient* sec);
    ~CSecurityTight() override;

    bool processMsg() override;
    int getType() const override { return secTypeTight; }
    int getEffectiveType() const override;
    bool isSecure() const override;

  private:
    bool isSubAuthAllowed(uint32_t subType) const;

    enum stateEnum {
      STATE_TUNNEL_COUNT,
      STATE_TUNNEL_LIST,
      STATE_AUTH_COUNT,
      STATE_AUTH_LIST,
      STATE_SUBAUTH
    };

    stateEnum state;
    SecurityClient* security;
    CSecurity* subSecurity;
    uint32_t nTunnels;
    uint32_t nAuthTypes;
  };

}

#endif
