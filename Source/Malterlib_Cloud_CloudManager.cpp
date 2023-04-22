// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_CloudManager.h"

namespace NMib::NCloud
{
	CCloudManager::CCloudManager()
	{
		DMibPublishActorFunction(CCloudManager::f_RegisterAppManager);
		DMibPublishActorFunction(CCloudManager::f_EnumAppManagers);
		DMibPublishActorFunction(CCloudManager::f_EnumApplications);
		DMibPublishActorFunction(CCloudManager::f_RemoveAppManager);
		DMibPublishActorFunction(CCloudManager::f_RemoveSensor);
		DMibPublishActorFunction(CCloudManager::f_RemoveLog);
		DMibPublishActorFunction(CCloudManager::f_GetSensorReporter);
		DMibPublishActorFunction(CCloudManager::f_GetSensorReader);
		DMibPublishActorFunction(CCloudManager::f_GetLogReporter);
		DMibPublishActorFunction(CCloudManager::f_GetLogReader);
		DMibPublishActorFunction(CCloudManager::f_SubscribeExpectedOsVersions);
		DMibPublishActorFunction(CCloudManager::f_EnumExpectedOsVersions);
		DMibPublishActorFunction(CCloudManager::f_SetExpectedOsVersions);
	}

	CCloudManager::~CCloudManager() = default;

	bool CCloudManager::CAppManagerDynamicInfo::f_HasErrors() const
	{
		return !m_bActive || !m_OtherErrors.f_IsEmpty();
	}

	uint32 CCloudManager::fs_ProtocolVersion_CloudManagerToAppManager(uint32 _CloudManagerVersion)
	{
		static_assert
			(
				CAppManagerInterface::EProtocolVersion_Current == CAppManagerInterface::EProtocolVersion_ResumableUpdateNotifications
				, "Add a new version mapping if streaming of m_ApplicationInfo changed"
			)
		;

		if (_CloudManagerVersion >= ECloudManagerProtocolVersion_AppManagerVersionIncreased4)
			return CAppManagerInterface::EProtocolVersion_ResumableUpdateNotifications;
		else if (_CloudManagerVersion >= ECloudManagerProtocolVersion_AppManagerVersionIncreased3)
			return CAppManagerInterface::EProtocolVersion_HostIDInApplicationInfo;
		else if (_CloudManagerVersion >= ECloudManagerProtocolVersion_AppManagerVersionIncreased2)
			return CAppManagerInterface::EProtocolVersion_AddLaunchInProcess;
		else if (_CloudManagerVersion >= ECloudManagerProtocolVersion_AppManagerVersionIncreased1)
			return CAppManagerInterface::EProtocolVersion_AddAutoUpdateAndExtendAppInfo;

		return CAppManagerInterface::EProtocolVersion_AddStatusSeverity;
	}

	bool CCloudManager::CExpectedVersionRange::f_IsSet() const
	{
		return m_Min || m_Max;
	}

	bool CCloudManager::CExpectedVersionRange::f_IsDeprecated() const
	{
		return m_Min && m_Max && m_Min->m_Major == TCLimitsInt<uint32>::mc_Max && m_Max->m_Major == TCLimitsInt<uint32>::mc_Max;
	}

	void CCloudManager::CExpectedVersionRange::f_SetDeprecated()
	{
		m_Min = CVersion{.m_Major = TCLimitsInt<uint32>::mc_Max};
		m_Max = CVersion{.m_Major = TCLimitsInt<uint32>::mc_Max};
	}

	void CCloudManager::CExpectedVersions::f_ApplyChanges(CExpectedVersions const &_Changes)
	{
		for (auto &Change : _Changes.m_Versions)
		{
			auto &Key = _Changes.m_Versions.fs_GetKey(Change);

			if (!Change.f_IsSet())
				m_Versions.f_Remove(Key);
			else
				m_Versions[Key] = Change;
		}
	}

	auto CCloudManager::CVersion::fs_ParseVersion(NStr::CStr _Version) -> NConcurrency::TCFuture<CVersion>
	{
		CVersion Version;

		auto *pParse = _Version.f_GetStr();

		auto fParsePoint = [&]() -> uint32
			{
				auto *pParseStart = pParse;

				auto fParseString = [](NStr::CStr const &_String, bool _bParsedPoint) -> uint32
					{
						if (_String.f_IsEmpty() && !_bParsedPoint)
							return 0;
						return _String.f_ToInt(TCLimitsInt<uint32>::mc_Max);
					}
				;

				while (*pParse)
				{
					if (*pParse == '.')
					{
						NStr::CStr Return(pParseStart, pParse - pParseStart);
						++pParse;
						return fParseString(Return, true);
					}

					++pParse;
				}

				return fParseString(NStr::CStr(pParseStart, pParse - pParseStart), false);
			}
		;

		Version.m_Major = fParsePoint();
		Version.m_Minor = fParsePoint();
		Version.m_Revision = fParsePoint();

		if (Version.m_Major == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("Failed to parse major version");
		if (Version.m_Minor == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("Failed to parse minor version");
		if (Version.m_Revision == TCLimitsInt<uint32>::mc_Max)
			co_return DMibErrorInstance("Failed to parse revision version");

		co_return fg_Move(Version);
	}
}
