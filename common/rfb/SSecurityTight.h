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

#ifndef __RFB_SSECURITYTIGHT_H__
#define __RFB_SSECURITYTIGHT_H__

#include <stdint.h>

#include <vector>

#include <rfb/SSecurity.h>
#include <rfb/Security.h>
#include <rfb/TightCapability.h>

namespace rfb {

  class SecurityServer;

  class SSecurityTight : public SSecurity {
  public:
    SSecurityTight(SConnection* sc, SecurityServer* sec);
    ~SSecurityTight() override;

    bool processMsg() override;
    int getType() const override { return secTypeTight; }
    int getEffectiveType() const override;
    const char* getUserName() const override;
    AccessRights getAccessRights() const override;

  private:
    void buildAuthCapabilities();

    enum stateEnum {
      STATE_SEND_CAPS,
      STATE_AUTH_CHOICE,
      STATE_SUBAUTH
    };

    stateEnum state;
    SecurityServer* security;
    SSecurity* subSecurity;
    std::vector<TightCapability> authCaps;
  };

}

#endif
