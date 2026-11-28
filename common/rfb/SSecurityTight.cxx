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

#include <algorithm>
#include <list>

#include <core/LogWriter.h>
#include <core/i18n.h>

#include <rdr/InStream.h>
#include <rdr/OutStream.h>

#include <rfb/Exception.h>
#include <rfb/SConnection.h>
#include <rfb/SSecurityTight.h>
#include <rfb/SecurityServer.h>

using namespace rfb;

static core::LogWriter vlog("STight");

static TightCapability makeCap(int32_t code, const char* vendor,
                               const char* signature)
{
  TightCapability cap;
  cap.code = code;
  memcpy(cap.vendor, vendor, 4);
  memcpy(cap.signature, signature, 8);
  return cap;
}

static void writeCapability(rdr::OutStream* os, const TightCapability& cap)
{
  os->writeS32(cap.code);
  os->writeBytes((const uint8_t*)cap.vendor, 4);
  os->writeBytes((const uint8_t*)cap.signature, 8);
}

SSecurityTight::SSecurityTight(SConnection* sc_, SecurityServer* sec)
  : SSecurity(sc_),
    state(STATE_SEND_CAPS),
    security(sec),
    subSecurity(nullptr)
{
}

SSecurityTight::~SSecurityTight()
{
  delete subSecurity;
}

int SSecurityTight::getEffectiveType() const
{
  if (subSecurity != nullptr)
    return subSecurity->getEffectiveType();
  return secTypeNone;
}

const char* SSecurityTight::getUserName() const
{
  if (subSecurity == nullptr)
    return "";
  return subSecurity->getUserName();
}

AccessRights SSecurityTight::getAccessRights() const
{
  if (subSecurity == nullptr)
    return SSecurity::getAccessRights();
  return subSecurity->getAccessRights();
}

void SSecurityTight::buildAuthCapabilities()
{
  authCaps.clear();
  if (security == nullptr)
    return;

  auto appendUnique = [this](int32_t code, const char* vendor, const char* sig) {
    for (const TightCapability& existing : authCaps) {
      if (existing.code == code)
        return;
    }
    authCaps.push_back(makeCap(code, vendor, sig));
  };

  std::list<uint8_t> secTypes = security->GetEnabledSecTypes();
  for (uint8_t type : secTypes) {
    if (type == secTypeVeNCrypt && !security->GetEnabledExtSecTypes().empty()) {
      appendUnique(tightAuthVeNCrypt, tightVendorVeNCrypt,
                   sigTightAuthVeNCrypt);
    } else if (type == secTypeVncAuth) {
      appendUnique(tightAuthVnc, tightVendorStandard, sigTightAuthVnc);
    } else if (type == secTypeNone) {
      appendUnique(tightAuthNone, tightVendorStandard, sigTightAuthNone);
    }
  }

  if (security->IsSupported(secTypePlain)) {
    appendUnique(tightAuthUnixLogin, tightVendorTight, sigTightAuthUnixLogin);
  }
}

bool SSecurityTight::processMsg()
{
  rdr::InStream* is = sc->getInStream();
  rdr::OutStream* os = sc->getOutStream();

  if (state == STATE_SEND_CAPS) {
    buildAuthCapabilities();
    if (authCaps.empty())
      throw protocol_error(_("No supported Tight authentication method"));

    // Offer zero tunneling capabilities (no tunneling)
    os->writeU32(0);

    os->writeU32(static_cast<uint32_t>(authCaps.size()));
    for (const TightCapability& cap : authCaps)
      writeCapability(os, cap);
    os->flush();

    state = STATE_AUTH_CHOICE;
    return false;
  }

  if (state == STATE_AUTH_CHOICE) {
    if (!is->hasData(4))
      return false;

    int32_t chosenCode = is->readS32();

    bool offered = false;
    for (const TightCapability& cap : authCaps) {
      if (cap.code == chosenCode) {
        offered = true;
        break;
      }
    }
    if (!offered)
      throw protocol_error(_("Unsupported Tight authentication scheme"));

    uint32_t mappedType = secTypeInvalid;
    switch (chosenCode) {
    case tightAuthNone:
      mappedType = secTypeNone;
      break;
    case tightAuthVnc:
      mappedType = secTypeVncAuth;
      break;
    case tightAuthVeNCrypt:
      mappedType = secTypeVeNCrypt;
      break;
    case tightAuthUnixLogin:
      mappedType = secTypePlain;
      break;
    default:
      throw protocol_error(_("Unsupported Tight authentication scheme"));
    }

    vlog.info(_("Client chooses Tight authentication scheme %d"),
              (int)chosenCode);

    subSecurity = security->GetSSecurity(sc, mappedType);
    state = STATE_SUBAUTH;
  }

  return subSecurity->processMsg();
}
