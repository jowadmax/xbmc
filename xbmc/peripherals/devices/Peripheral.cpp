/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "Peripheral.h"
#include "peripherals/Peripherals.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "settings/Setting.h"
#include "utils/XBMCTinyXML.h"
#include "utils/URIUtils.h"
#include "guilib/LocalizeStrings.h"

using namespace PERIPHERALS;
using namespace std;

CPeripheral::CPeripheral(const PeripheralScanResult& scanResult) :
  m_type(scanResult.m_mappedType),
  m_busType(scanResult.m_busType),
  m_mappedBusType(scanResult.m_mappedBusType),
  m_strLocation(scanResult.m_strLocation),
  m_strDeviceName(scanResult.m_strDeviceName),
  m_strFileLocation(StringUtils::EmptyString),
  m_iVendorId(scanResult.m_iVendorId),
  m_iProductId(scanResult.m_iProductId),
  m_strVersionInfo(g_localizeStrings.Get(13205)), // "unknown"
  m_bInitialised(false),
  m_bHidden(false),
  m_bError(false)
{
  PeripheralTypeTranslator::FormatHexString(scanResult.m_iVendorId, m_strVendorId);
  PeripheralTypeTranslator::FormatHexString(scanResult.m_iProductId, m_strProductId);
  m_strFileLocation.Format(scanResult.m_iSequence > 0 ? "peripherals://%s/%s_%d.dev" : "peripherals://%s/%s.dev", PeripheralTypeTranslator::BusTypeToString(scanResult.m_busType), scanResult.m_strLocation.c_str(), scanResult.m_iSequence);
}

CPeripheral::~CPeripheral(void)
{
  PersistSettings(true);

  for (unsigned int iSubdevicePtr = 0; iSubdevicePtr < m_subDevices.size(); iSubdevicePtr++)
    delete m_subDevices.at(iSubdevicePtr);
  m_subDevices.clear();

  ClearSettings();
}

bool CPeripheral::operator ==(const CPeripheral &right) const
{
  return m_type == right.m_type &&
      m_strLocation == right.m_strLocation &&
      m_iVendorId == right.m_iVendorId &&
      m_iProductId == right.m_iProductId;
}

bool CPeripheral::operator !=(const CPeripheral &right) const
{
  return !(*this == right);
}

bool CPeripheral::HasFeature(const PeripheralFeature feature) const
{
  bool bReturn(false);

  for (unsigned int iFeaturePtr = 0; iFeaturePtr < m_features.size(); iFeaturePtr++)
  {
    if (m_features.at(iFeaturePtr) == feature)
    {
      bReturn = true;
      break;
    }
  }

  if (!bReturn)
  {
    for (unsigned int iSubdevicePtr = 0; iSubdevicePtr < m_subDevices.size(); iSubdevicePtr++)
    {
      if (m_subDevices.at(iSubdevicePtr)->HasFeature(feature))
      {
        bReturn = true;
        break;
      }
    }
  }

  return bReturn;
}

void CPeripheral::GetFeatures(std::vector<PeripheralFeature> &features) const
{
  for (unsigned int iFeaturePtr = 0; iFeaturePtr < m_features.size(); iFeaturePtr++)
    features.push_back(m_features.at(iFeaturePtr));

  for (unsigned int iSubdevicePtr = 0; iSubdevicePtr < m_subDevices.size(); iSubdevicePtr++)
    m_subDevices.at(iSubdevicePtr)->GetFeatures(features);
}

bool CPeripheral::Initialise(void)
{
  bool bReturn(false);

  if (m_bError)
    return bReturn;

  bReturn = true;
  if (m_bInitialised)
    return bReturn;

  g_peripherals.GetSettingsFromMapping(*this);
  m_strSettingsFile.Format("special://profile/peripheral_data/%s_%s_%s.xml", PeripheralTypeTranslator::BusTypeToString(m_mappedBusType), m_strVendorId.c_str(), m_strProductId.c_str());
  LoadPersistedSettings();

  for (unsigned int iFeaturePtr = 0; iFeaturePtr < m_features.size(); iFeaturePtr++)
  {
    PeripheralFeature feature = m_features.at(iFeaturePtr);
    bReturn &= InitialiseFeature(feature);
  }

  for (unsigned int iSubdevicePtr = 0; iSubdevicePtr < m_subDevices.size(); iSubdevicePtr++)
    bReturn &= m_subDevices.at(iSubdevicePtr)->Initialise();

  if (bReturn)
  {
    CLog::Log(LOGDEBUG, "%s - initialised peripheral on '%s' with %d features and %d sub devices",
      __FUNCTION__, m_strLocation.c_str(), (int)m_features.size(), (int)m_subDevices.size());
    m_bInitialised = true;
  }

  return bReturn;
}

void CPeripheral::GetSubdevices(vector<CPeripheral *> &subDevices) const
{
  for (unsigned int iSubdevicePtr = 0; iSubdevicePtr < m_subDevices.size(); iSubdevicePtr++)
    subDevices.push_back(m_subDevices.at(iSubdevicePtr));
}

bool CPeripheral::IsMultiFunctional(void) const
{
  return m_subDevices.size() > 0;
}

vector<CSetting *> CPeripheral::GetSettings(void) const
{
  vector<CSetting *> settings;
  for (map<CStdString, CSetting *>::const_iterator it = m_settings.begin(); it != m_settings.end(); it++)
    settings.push_back(it->second);
  return settings;
}

void CPeripheral::AddSetting(const CStdString &strKey, const CSetting *setting)
{
  if (!setting)
  {
    CLog::Log(LOGERROR, "%s - invalid setting", __FUNCTION__);
    return;
  }

  if (!HasSetting(strKey))
  {
    switch(setting->GetType())
    {
    case SettingTypeBool:
      {
        const CSettingBool *mappedSetting = (const CSettingBool *) setting;
        CSettingBool *boolSetting = new CSettingBool(strKey, *mappedSetting);
        if (boolSetting)
        {
          boolSetting->SetVisible(mappedSetting->IsVisible());
          m_settings.insert(make_pair(strKey, boolSetting));
        }
      }
      break;
    case SettingTypeInteger:
      {
        const CSettingInt *mappedSetting = (const CSettingInt *) setting;
        CSettingInt *intSetting = new CSettingInt(strKey, *mappedSetting);
        if (intSetting)
        {
          intSetting->SetVisible(mappedSetting->IsVisible());
          m_settings.insert(make_pair(strKey, intSetting));
        }
      }
      break;
    case SettingTypeNumber:
      {
        const CSettingNumber *mappedSetting = (const CSettingNumber *) setting;
        CSettingNumber *floatSetting = new CSettingNumber(strKey, *mappedSetting);
        if (floatSetting)
        {
          floatSetting->SetVisible(mappedSetting->IsVisible());
          m_settings.insert(make_pair(strKey, floatSetting));
        }
      }
      break;
    case SettingTypeString:
      {
        const CSettingString *mappedSetting = (const CSettingString *) setting;
        CSettingString *stringSetting = new CSettingString(strKey, *mappedSetting);
        if (stringSetting)
        {
          stringSetting->SetVisible(mappedSetting->IsVisible());
          m_settings.insert(make_pair(strKey, stringSetting));
        }
      }
      break;
    default:
      //TODO add more types if needed
      break;
    }
  }
}

bool CPeripheral::HasSetting(const CStdString &strKey) const
{
  map<CStdString, CSetting *>:: const_iterator it = m_settings.find(strKey);
  return it != m_settings.end();
}

bool CPeripheral::HasSettings(void) const
{
  return m_settings.size() > 0;
}

bool CPeripheral::HasConfigurableSettings(void) const
{
  bool bReturn(false);
  map<CStdString, CSetting *>::const_iterator it = m_settings.begin();
  while (it != m_settings.end() && !bReturn)
  {
    if ((*it).second->IsVisible())
    {
      bReturn = true;
      break;
    }

    ++it;
  }

  return bReturn;
}

bool CPeripheral::GetSettingBool(const CStdString &strKey) const
{
  map<CStdString, CSetting *>::const_iterator it = m_settings.find(strKey);
  if (it != m_settings.end() && (*it).second->GetType() == SettingTypeBool)
  {
      CSettingBool *boolSetting = (CSettingBool *) (*it).second;
    if (boolSetting)
      return boolSetting->GetValue();
  }

  return false;
}

int CPeripheral::GetSettingInt(const CStdString &strKey) const
{
  map<CStdString, CSetting *>::const_iterator it = m_settings.find(strKey);
  if (it != m_settings.end() && (*it).second->GetType() == SettingTypeInteger)
  {
      CSettingInt *intSetting = (CSettingInt *) (*it).second;
    if (intSetting)
      return intSetting->GetValue();
  }

  return 0;
}

float CPeripheral::GetSettingFloat(const CStdString &strKey) const
{
  map<CStdString, CSetting *>::const_iterator it = m_settings.find(strKey);
  if (it != m_settings.end() && (*it).second->GetType() == SettingTypeNumber)
  {
      CSettingNumber *floatSetting = (CSettingNumber *) (*it).second;
    if (floatSetting)
      return (float)floatSetting->GetValue();
  }

  return 0;
}

const CStdString CPeripheral::GetSettingString(const CStdString &strKey) const
{
  map<CStdString, CSetting *>::const_iterator it = m_settings.find(strKey);
  if (it != m_settings.end() && (*it).second->GetType() == SettingTypeString)
  {
      CSettingString *stringSetting = (CSettingString *) (*it).second;
    if (stringSetting)
      return stringSetting->GetValue();
  }

  return StringUtils::EmptyString;
}

bool CPeripheral::SetSetting(const CStdString &strKey, bool bValue)
{
  bool bChanged(false);
  map<CStdString, CSetting *>::iterator it = m_settings.find(strKey);
  if (it != m_settings.end() && (*it).second->GetType() == SettingTypeBool)
  {
      CSettingBool *boolSetting = (CSettingBool *) (*it).second;
    if (boolSetting)
    {
      bChanged = boolSetting->GetValue() != bValue;
      boolSetting->SetValue(bValue);
      if (bChanged && m_bInitialised)
        m_changedSettings.insert(strKey);
    }
  }
  return bChanged;
}

bool CPeripheral::SetSetting(const CStdString &strKey, int iValue)
{
  bool bChanged(false);
  map<CStdString, CSetting *>::iterator it = m_settings.find(strKey);
  if (it != m_settings.end() && (*it).second->GetType() == SettingTypeInteger)
  {
      CSettingInt *intSetting = (CSettingInt *) (*it).second;
    if (intSetting)
    {
      bChanged = intSetting->GetValue() != iValue;
      intSetting->SetValue(iValue);
      if (bChanged && m_bInitialised)
        m_changedSettings.insert(strKey);
    }
  }
  return bChanged;
}

bool CPeripheral::SetSetting(const CStdString &strKey, float fValue)
{
  bool bChanged(false);
  map<CStdString, CSetting *>::iterator it = m_settings.find(strKey);
  if (it != m_settings.end() && (*it).second->GetType() == SettingTypeNumber)
  {
      CSettingNumber *floatSetting = (CSettingNumber *) (*it).second;
    if (floatSetting)
    {
      bChanged = floatSetting->GetValue() != fValue;
      floatSetting->SetValue(fValue);
      if (bChanged && m_bInitialised)
        m_changedSettings.insert(strKey);
    }
  }
  return bChanged;
}

void CPeripheral::SetSettingVisible(const CStdString &strKey, bool bSetTo)
{
  map<CStdString, CSetting *>::iterator it = m_settings.find(strKey);
  if (it != m_settings.end())
    (*it).second->SetVisible(bSetTo);
}

bool CPeripheral::IsSettingVisible(const CStdString &strKey) const
{
  map<CStdString, CSetting *>::const_iterator it = m_settings.find(strKey);
  if (it != m_settings.end())
    return (*it).second->IsVisible();
  return false;
}

bool CPeripheral::SetSetting(const CStdString &strKey, const CStdString &strValue)
{
  bool bChanged(false);
  map<CStdString, CSetting *>::iterator it = m_settings.find(strKey);
  if (it != m_settings.end())
  {
    if ((*it).second->GetType() == SettingTypeString)
    {
        CSettingString *stringSetting = (CSettingString *) (*it).second;
      if (stringSetting)
      {
        bChanged = !StringUtils::EqualsNoCase(stringSetting->GetValue(), strValue);
        stringSetting->SetValue(strValue);
        if (bChanged && m_bInitialised)
          m_changedSettings.insert(strKey);
      }
    }
    else if ((*it).second->GetType() == SettingTypeInteger)
      bChanged = SetSetting(strKey, (int) (strValue.IsEmpty() ? 0 : atoi(strValue.c_str())));
    else if ((*it).second->GetType() == SettingTypeNumber)
      bChanged = SetSetting(strKey, (float) (strValue.IsEmpty() ? 0 : atof(strValue.c_str())));
    else if ((*it).second->GetType() == SettingTypeBool)
      bChanged = SetSetting(strKey, strValue.Equals("1"));
  }
  return bChanged;
}

void CPeripheral::PersistSettings(bool bExiting /* = false */)
{
  CXBMCTinyXML doc;
  TiXmlElement node("settings");
  doc.InsertEndChild(node);
  for (map<CStdString, CSetting *>::const_iterator itr = m_settings.begin(); itr != m_settings.end(); itr++)
  {
    TiXmlElement nodeSetting("setting");
    nodeSetting.SetAttribute("id", itr->first.c_str());
    CStdString strValue;
    switch ((*itr).second->GetType())
    {
    case SettingTypeString:
      {
        CSettingString *stringSetting = (CSettingString *) (*itr).second;
        if (stringSetting)
          strValue = stringSetting->GetValue();
      }
      break;
    case SettingTypeInteger:
      {
        CSettingInt *intSetting = (CSettingInt *) (*itr).second;
        if (intSetting)
          strValue.Format("%d", intSetting->GetValue());
      }
      break;
    case SettingTypeNumber:
      {
        CSettingNumber *floatSetting = (CSettingNumber *) (*itr).second;
        if (floatSetting)
          strValue.Format("%.2f", floatSetting->GetValue());
      }
      break;
    case SettingTypeBool:
      {
        CSettingBool *boolSetting = (CSettingBool *) (*itr).second;
        if (boolSetting)
          strValue.Format("%d", boolSetting->GetValue() ? 1:0);
      }
      break;
    default:
      break;
    }
    nodeSetting.SetAttribute("value", strValue.c_str());
    doc.RootElement()->InsertEndChild(nodeSetting);
  }

  doc.SaveFile(m_strSettingsFile);

  if (!bExiting)
  {
    for (set<CStdString>::const_iterator it = m_changedSettings.begin(); it != m_changedSettings.end(); it++)
      OnSettingChanged(*it);
  }
  m_changedSettings.clear();
}

void CPeripheral::LoadPersistedSettings(void)
{
  CXBMCTinyXML doc;
  if (doc.LoadFile(m_strSettingsFile))
  {
    const TiXmlElement *setting = doc.RootElement()->FirstChildElement("setting");
    while (setting)
    {
      CStdString strId(setting->Attribute("id"));
      CStdString strValue(setting->Attribute("value"));
      SetSetting(strId, strValue);

      setting = setting->NextSiblingElement("setting");
    }
  }
}

void CPeripheral::ResetDefaultSettings(void)
{
  ClearSettings();
  g_peripherals.GetSettingsFromMapping(*this);

  map<CStdString, CSetting *>::iterator it = m_settings.begin();
  while (it != m_settings.end())
  {
    m_changedSettings.insert((*it).first);
    ++it;
  }

  PersistSettings();
}

void CPeripheral::ClearSettings(void)
{
  map<CStdString, CSetting *>::iterator it = m_settings.begin();
  while (it != m_settings.end())
  {
    delete (*it).second;
    ++it;
  }
  m_settings.clear();
}

bool CPeripheral::operator ==(const PeripheralScanResult& right) const
{
  return m_strLocation.Equals(right.m_strLocation);
}

bool CPeripheral::operator !=(const PeripheralScanResult& right) const
{
  return !(*this == right);
}
