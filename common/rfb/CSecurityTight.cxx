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

#include <string.h>

#include <vector>

#include <core/LogWriter.h>
#include <core/i18n.h>

#include <rdr/InStream.h>
#include <rdr/OutStream.h>

#include <rfb/CConnection.h>
#include <rfb/CSecurityNone.h>
#include <rfb/CSecurityPlain.h>
#include <rfb/CSecurityTight.h>
#include <rfb/CSecurityVeNCrypt.h>
#include <rfb/CSecurityVncAuth.h>
#include <rfb/Exception.h>
#include <rfb/SecurityClient.h>
#include <rfb/TightCapability.h>

using namespace rfb;

static core::LogWriter vlog("CSecurityTight");

static const uint32_t maxTightCapabilities = 256;

CSecurityTight::CSecurityTight(CConnection* cc_, SecurityClient* sec)
  : CSecurity(cc_),
    state(STATE_TUNNEL_COUNT),
    security(sec),
    subSecurity(nullptr),
    nTunnels(0),
    nAuthTypes(0)
{
}

CSecurityTight::~CSecurityTight()
{
  delete subSecurity;
}

int CSecurityTight::getEffectiveType() const
{
  if (subSecurity != nullptr)
    return subSecurity->getEffectiveType();
  return secTypeNone;
}

bool CSecurityTight::isSecure() const
{
  if (subSecurity != nullptr)
    return subSecurity->isSecure();
  return false;
}

bool CSecurityTight::isSubAuthAllowed(uint32_t subType) const
{
  if (security == nullptr)
    return true;

  if (security->IsSupported(subType))
    return true;

  // A client configured with Tight alone accepts the standard Tight schemes.
  return security->GetEnabledExtSecTypes().empty() &&
         (subType == secTypeNone || subType == secTypeVncAuth ||
          subType == secTypePlain);
}

static void readCapability(rdr::InStream* is, TightCapability* cap)
{
  cap->code = is->readS32();
  is->readBytes((uint8_t*)cap->vendor, 4);
  cap->vendor[4] = '\0';
  is->readBytes((uint8_t*)cap->signature, 8);
  cap->signature[8] = '\0';
}

bool CSecurityTight::processMsg()
{
  rdr::InStream* is = cc->getInStream();
  rdr::OutStream* os = cc->getOutStream();

  if (state == STATE_TUNNEL_COUNT) {
    if (!is->hasData(4))
      return false;

    nTunnels = is->readU32();
    if (nTunnels > maxTightCapabilities)
      throw protocol_error(_("Too many Tight tunneling capabilities"));

    vlog.debug("Server offers %u Tight tunnel capabilities", nTunnels);

    if (nTunnels == 0)
      state = STATE_AUTH_COUNT;
    else
      state = STATE_TUNNEL_LIST;
  }

  if (state == STATE_TUNNEL_LIST) {
    if (!is->hasData((size_t)nTunnels * 16))
      return false;

    bool foundNoTunnel = false;
    for (uint32_t i = 0; i < nTunnels; i++) {
      TightCapability cap;
      readCapability(is, &cap);
      vlog.debug("  Tunnel %d: %.4s / %.8s",
                 (int)cap.code, cap.vendor, cap.signature);
      if (cap.matches(tightTunnelNone, tightVendorTight, sigTightTunnelNone))
        foundNoTunnel = true;
    }

    if (!foundNoTunnel)
      throw protocol_error(_("No supported Tight tunneling method"));

    os->writeS32(tightTunnelNone);
    os->flush();
    state = STATE_AUTH_COUNT;
  }

  if (state == STATE_AUTH_COUNT) {
    if (!is->hasData(4))
      return false;

    nAuthTypes = is->readU32();
    if (nAuthTypes > maxTightCapabilities)
      throw protocol_error(_("Too many Tight authentication capabilities"));

    vlog.debug("Server offers %u Tight authentication capabilities", nAuthTypes);

    if (nAuthTypes == 0) {
      if (!isSubAuthAllowed(secTypeNone))
        throw protocol_error(_("No supported Tight authentication method"));
      subSecurity = new CSecurityNone(cc);
      state = STATE_SUBAUTH;
    } else {
      state = STATE_AUTH_LIST;
    }
  }

  if (state == STATE_AUTH_LIST) {
    if (!is->hasData((size_t)nAuthTypes * 16))
      return false;

    std::vector<TightCapability> caps(nAuthTypes);
    for (uint32_t i = 0; i < nAuthTypes; i++) {
      readCapability(is, &caps[i]);
      vlog.debug("  Auth %d: %.4s / %.8s",
                 (int)caps[i].code, caps[i].vendor, caps[i].signature);
    }

    int32_t chosenCode = 0;
    CSecurity* chosen = nullptr;

    for (uint32_t i = 0; i < nAuthTypes && chosen == nullptr; i++) {
      const TightCapability& cap = caps[i];
      if (cap.matches(tightAuthNone, tightVendorStandard, sigTightAuthNone) &&
          isSubAuthAllowed(secTypeNone)) {
        chosenCode = cap.code;
        chosen = new CSecurityNone(cc);
      } else if (cap.matches(tightAuthVnc, tightVendorStandard, sigTightAuthVnc) &&
                 isSubAuthAllowed(secTypeVncAuth)) {
        chosenCode = cap.code;
        chosen = new CSecurityVncAuth(cc);
      } else if (cap.matches(tightAuthVeNCrypt, tightVendorVeNCrypt, sigTightAuthVeNCrypt) &&
                 security != nullptr && security->IsSupported(secTypeVeNCrypt)) {
        chosenCode = cap.code;
        chosen = new CSecurityVeNCrypt(cc, security);
      } else if (cap.matches(tightAuthUnixLogin, tightVendorTight, sigTightAuthUnixLogin) &&
                 isSubAuthAllowed(secTypePlain)) {
        chosenCode = cap.code;
        chosen = new CSecurityPlain(cc);
      }
    }

    if (chosen == nullptr)
      throw protocol_error(_("No supported Tight authentication method"));

    vlog.info(_("Choosing Tight authentication scheme %d"), (int)chosenCode);

    subSecurity = chosen;
    os->writeS32(chosenCode);
    os->flush();

    state = STATE_SUBAUTH;
  }

  return subSecurity->processMsg();
}
