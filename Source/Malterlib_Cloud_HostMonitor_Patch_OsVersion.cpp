// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

#include <Mib/Concurrency/LogError>

namespace NMib::NCloud
{
	TCFuture<void> CHostMonitor::f_SetExpectedOsVersions(CCloudManager::CExpectedVersions &&_ExpectedOsVersions)
	{
		auto &Internal = *mp_pInternal;
		Internal.m_ExpectedOsVersions = fg_Move(_ExpectedOsVersions);
		co_await Internal.f_PeriodicUpdate_Patch(false);
		co_return {};
	}

	TCFuture<CDistributedAppSensorReporter::CVersion> CHostMonitor::CInternal::f_GetOsNameAndVersion()
	{
		CDistributedAppSensorReporter::CVersion Return;

		Return.m_Identifier = DMibStringize(DPlatformFamily);

		int Major, Minor, Fix;
		EOperatingSystemArch Arch;
		NSys::fg_System_GetOperatingSystemVersion(Major, Minor, Fix, Arch);

		Return.m_Major = Major;
		Return.m_Minor = Minor;
		Return.m_Revision = Fix;

#if defined(DPlatformFamily_Linux)
		auto BlockingActorCheckout = fg_BlockingActor();
		co_await fg_ContinueRunningOnActor(BlockingActorCheckout);

		auto CaptureExceptions = co_await (g_CaptureExceptions.f_Specific<NFile::CExceptionFile>() % "Failed to get OS name");

		if (CFile::fs_FileExists(CStr("/etc/os-release")))
		{
			CStr Contents = CFile::fs_ReadStringFromFile(CStr("/etc/os-release"), true);

			CStr VersionString;
			mint nVersionStringDots = 0;

			auto fAddVersionString = [&](CStr const &_Version)
				{
					CStr VersionParse = _Version;
					CStr Version = fg_GetStrSep(VersionParse, " ");

					mint nDots = 0;
					for (auto const *pParse = _Version.f_GetStr(); *pParse; ++pParse)
					{
						if (*pParse == '.')
							++nDots;
					}

					if (nDots > nVersionStringDots)
					{
						nVersionStringDots = nDots;
						VersionString = Version;
					}
				}
			;

			for (auto &Line : Contents.f_SplitLine())
			{
				if (Line.f_StartsWith("NAME="))
					Return.m_Identifier = fg_RemoveEscape(Line.f_RemovePrefix("NAME="));
				else if (Line.f_StartsWith("VERSION_ID="))
					fAddVersionString(fg_RemoveEscape(Line.f_RemovePrefix("VERSION_ID=")));
				else if (Line.f_StartsWith("VERSION="))
					fAddVersionString(fg_RemoveEscape(Line.f_RemovePrefix("VERSION=")));
			}

			if (VersionString)
			{
				auto CloudManagerVersion = co_await CCloudManager::CVersion::fs_ParseVersion(VersionString).f_Wrap();
				if (CloudManagerVersion)
				{
					Return.m_Major = CloudManagerVersion->m_Major;
					Return.m_Minor = CloudManagerVersion->m_Minor;
					Return.m_Revision = CloudManagerVersion->m_Revision;
				}
			}
		}
		else if (CFile::fs_FileExists(CStr("/etc/SuSE-release")))
		{
			CStr Contents = CFile::fs_ReadStringFromFile(CStr("/etc/os-release"), true);

			auto Lines = Contents.f_SplitLine();

			if (!Lines.f_IsEmpty() && Lines[0])
				Return.m_Identifier = Lines[0];

			auto fFoundVersion = [&, bFound = false]() mutable
				{
					if (bFound)
						return;
					bFound = true;
					Return.m_Major = 0;
					Return.m_Minor = 0;
					Return.m_Revision = 0;
				}
			;

			for (auto &Line : Lines)
			{
				if (Line.f_StartsWith("VERSION = "))
				{
					fFoundVersion();
					Return.m_Major = Line.f_RemovePrefix("VERSION = ").f_ToInt(0);
				}
				else if (Line.f_StartsWith("PATCHLEVEL = "))
				{
					fFoundVersion();
					Return.m_Minor = Line.f_RemovePrefix("PATCHLEVEL = ").f_ToInt(0);
				}
			}
		}

#endif
		co_return fg_Move(Return);
	}

	TCFuture<bool> CHostMonitor::CInternal::f_PeriodicUpdate_Patch_OsVersion()
	{
		auto OnResume = co_await m_pThis->f_CheckDestroyedOnResume();

		CLogError LogError("Malterlib/Cloud/HostMonitor");

		if (!m_OsVersionReporter)
		{
			CDistributedAppSensorReporter::CSensorInfo SensorInfo;
			SensorInfo.m_Identifier = "org.malterlib.os.version";
			SensorInfo.m_Name = "OS Version";
			SensorInfo.m_Type = CDistributedAppSensorReporter::ESensorDataType_Version;
			SensorInfo.m_MetaData = m_SensorMetaData;
			if (m_Config.m_PatchInterval != 0.0)
				SensorInfo.m_ExpectedReportInterval = m_Config.m_PatchInterval;

			m_OsVersionReporter = co_await m_SensorStore(&CDistributedAppSensorStoreLocal::f_OpenSensorReporter, fg_Move(SensorInfo));
		}

		m_CurrentOsVersion = co_await f_GetOsNameAndVersion();

		co_await (*m_OsVersionReporter).m_fReportReadings(TCVector{CDistributedAppSensorReporter::CSensorReading{.m_Data = m_CurrentOsVersion}}).f_Wrap()
			> LogError("Failed to report reading (OS Version)")
		;

		co_return false;
	}

	TCFuture<bool> CHostMonitor::CInternal::f_PeriodicUpdate_Patch_ExpectedOsVersion()
	{
		auto OnResume = co_await m_pThis->f_CheckDestroyedOnResume();

		CLogError LogError("Malterlib/Cloud/HostMonitor");

		CCloudManager::CExpectedVersionRange *pExpectedVersionRange = nullptr;
		{
			CCloudManager::CCurrentVersion CurrentVersion;
			CurrentVersion.m_Major = m_CurrentOsVersion.m_Major;
			CurrentVersion.m_Minor = m_CurrentOsVersion.m_Minor;

			pExpectedVersionRange = m_ExpectedOsVersions.m_Versions.f_FindEqual(CurrentVersion);

			if (!pExpectedVersionRange)
			{
				CurrentVersion.m_Minor.f_Clear();
				pExpectedVersionRange = m_ExpectedOsVersions.m_Versions.f_FindEqual(CurrentVersion);
			}

			if (!pExpectedVersionRange)
			{
				CurrentVersion.m_Major.f_Clear();
				pExpectedVersionRange = m_ExpectedOsVersions.m_Versions.f_FindEqual(CurrentVersion);
			}
		}

		bool bUpdateDatabase = false;

		if (!m_OsVersionStatusReporter)
		{
			CDistributedAppSensorReporter::CSensorInfo SensorInfo;
			SensorInfo.m_Identifier = "org.malterlib.os.version.status";
			SensorInfo.m_Name = "OS Version Status";
			SensorInfo.m_Type = CDistributedAppSensorReporter::ESensorDataType_Status;
			SensorInfo.m_MetaData = m_SensorMetaData;
			if (m_Config.m_PatchInterval != 0.0)
				SensorInfo.m_ExpectedReportInterval = m_Config.m_PatchInterval;

			m_OsVersionStatusReporter = co_await m_SensorStore(&CDistributedAppSensorStoreLocal::f_OpenSensorReporter, fg_Move(SensorInfo));
		}

		CCloudManager::CVersion CloudManagerVersion;
		CloudManagerVersion.m_Major = m_CurrentOsVersion.m_Major;
		CloudManagerVersion.m_Minor = m_CurrentOsVersion.m_Minor;
		CloudManagerVersion.m_Revision = m_CurrentOsVersion.m_Revision;

		CDistributedAppSensorReporter::CStatus Status;

		if (pExpectedVersionRange)
		{
			if (pExpectedVersionRange->f_IsDeprecated())
			{
				Status.m_Severity = fg_Max(Status.m_Severity, CDistributedAppSensorReporter::EStatusSeverity_Warning);
				Status.m_Description = "The OS version '{}' is deprecated"_f << m_CurrentOsVersion;
			}
			else if (pExpectedVersionRange->m_Min && CloudManagerVersion < *pExpectedVersionRange->m_Min)
			{
				Status.m_Severity = fg_Max(Status.m_Severity, CDistributedAppSensorReporter::EStatusSeverity_Warning);
				Status.m_Description = "The OS version '{}' is older than the allowed lowest version '{}'"_f << m_CurrentOsVersion << *pExpectedVersionRange->m_Min;
			}
			else if (pExpectedVersionRange->m_Max && CloudManagerVersion > *pExpectedVersionRange->m_Max)
			{
				Status.m_Severity = fg_Max(Status.m_Severity, CDistributedAppSensorReporter::EStatusSeverity_Warning);
				Status.m_Description = "The OS version '{}' is newer than the allowed higest version '{}'"_f << m_CurrentOsVersion << *pExpectedVersionRange->m_Max;
			}
		}

		if (Status.m_Severity >= CDistributedAppSensorReporter::EStatusSeverity_Warning)
		{
			if (!m_PatchDatabaseState.m_ProblemStart_ExpectedOsVersionError.f_IsValid())
			{
				bUpdateDatabase = true;
				m_PatchDatabaseState.m_ProblemStart_ExpectedOsVersionError = CTime::fs_NowUTC();
			}
			else if ((CTime::fs_NowUTC() - m_PatchDatabaseState.m_ProblemStart_ExpectedOsVersionError).f_GetSecondsFraction() >= m_Config.m_ReportErrorAfter_OsVersion)
				Status.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Error;
		}
		else
		{
			if (pExpectedVersionRange)
			{
				Status.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Ok;
				Status.m_Description = "OS is up to date";
			}
			else
			{
				Status.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Info;
				Status.m_Description = "Expected OS version not specified";
			}

			if (m_PatchDatabaseState.m_ProblemStart_ExpectedOsVersionError.f_IsValid())
			{
				bUpdateDatabase = true;
				m_PatchDatabaseState.m_ProblemStart_ExpectedOsVersionError = CTime();
			}
		}

		CDistributedAppSensorReporter::CSensorReading Reading;
		Reading.m_Data = fg_Move(Status);

		co_await m_OsVersionStatusReporter->m_fReportReadings(TCVector<CDistributedAppSensorReporter::CSensorReading>{fg_Move(Reading)}).f_Wrap()
			> LogError("Failed to report readings (Expected OS Version)")
		;

		co_return bUpdateDatabase;
	}
}
