// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Process/ProcessLaunchActor>

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

namespace NMib::NCloud
{
	using namespace NHostMonitorDatabase;
	using namespace NProcess;

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
		co_await fg_ContinueRunningOnActor(m_FileActor);

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

	TCFuture<void> CHostMonitor::CInternal::f_PeriodicUpdate_Patch(bool _bCanSkip)
	{
		if (!m_Config.m_PatchInterval)
			co_return {};

		auto OnResume = co_await m_pThis->f_CheckDestroyedOnResume();

		auto SequenceSubscription = co_await m_UpdatePeriodicPatch.f_Sequence();

		bool bIsOverTime = m_PatchClock && (*m_PatchClock).f_GetTime() >= m_Config.m_PatchInterval;

		if (!m_PatchClock)
			m_PatchClock = CClock{true};
		else if (_bCanSkip && !bIsOverTime)
			co_return {};

		if (bIsOverTime)
			(*m_PatchClock).f_AddOffset(m_Config.m_PatchInterval);

		CLogError LogError("Malterlib/Cloud/HostMonitor");

		bool bUpdateDatabase = co_await f_PeriodicUpdate_Patch_OsVersion();
		bUpdateDatabase = bUpdateDatabase || co_await f_PeriodicUpdate_Patch_ExpectedOsVersion();
		bUpdateDatabase = bUpdateDatabase || co_await f_PeriodicUpdate_Patch_PatchStatus();

		if (bUpdateDatabase)
		{
			co_await m_Database
				(
					&CDatabaseActor::f_WriteWithCompaction
					, g_ActorFunctorWeak / [this](CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
					{
						auto CaptureScope = co_await (g_CaptureExceptions % "Error saving patch state to database");

						// TODO: Handle _bCompacting

						auto WriteTransaction = fg_Move(_Transaction);

						CPatchStateKey Key;
						WriteTransaction.m_Transaction.f_Upsert(Key, m_PatchDatabaseState);

						co_return fg_Move(WriteTransaction);
					}
				)
				.f_Wrap() > LogError("Error saving patch state to database")
			;
		}

		co_return {};
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

	TCFuture<bool> CHostMonitor::CInternal::f_PeriodicUpdate_Patch_PatchStatus()
	{
#ifndef DPlatformFamily_Linux
		co_return {};
#else

		auto OnResume = co_await m_pThis->f_CheckDestroyedOnResume();

		CLogError LogError("Malterlib/Cloud/HostMonitor");

		TCActor<CProcessLaunchActor> AptCheckLaunchActor;

		auto Cleanup = g_OnScopeExit / [&]
			{
				if (AptCheckLaunchActor)
					fg_Move(AptCheckLaunchActor).f_Destroy() > LogError("Failed to destroy process launch");
			}
		;

		CStr AptCheckExecutable = "/usr/lib/update-notifier/apt-check";

		struct CFileProperties
		{
			bool m_bAptCheckExists = false;
			bool m_bRebootRequired = false;
		};

		auto FileProperties = co_await
			(
				g_Dispatch(m_FileActor) / [=]() -> TCFuture<CFileProperties>
				{
					auto CaptureExceptions = co_await (g_CaptureExceptions.f_Specific<NFile::CExceptionFile>() % "Failed read patch status from files");

					CStr RebootRequiredFile = "/var/run/reboot-required";

					CFileProperties Return;
					Return.m_bAptCheckExists = CFile::fs_FileExists(AptCheckExecutable);
					Return.m_bRebootRequired = CFile::fs_FileExists(RebootRequiredFile) && !CFile::fs_ReadStringFromFile(RebootRequiredFile, true).f_Trim().f_IsEmpty();

					co_return fg_Move(Return);
				}
			)
		;

		uint32 nSecurityPatches = 0;
		uint32 nNormalPatches = 0;

		if (FileProperties.m_bAptCheckExists)
		{
			AptCheckLaunchActor = fg_Construct();

			CProcessLaunchActor::CSimpleLaunch Launch
				{
					AptCheckExecutable
					, {}
					, CFile::fs_GetPath(AptCheckExecutable)
					, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
				}
			;

			Launch.m_ToLog = CProcessLaunchActor::ELogFlag_Error;

			auto PatchesNeeded = (co_await AptCheckLaunchActor(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch))).f_GetStdErr().f_Trim().f_Split<true>(";");

			if (PatchesNeeded.f_GetLen() >= 1)
				nNormalPatches = PatchesNeeded[0].f_ToInt(uint32(0));

			if (PatchesNeeded.f_GetLen() >= 2)
				nSecurityPatches = PatchesNeeded[1].f_ToInt(uint32(0));
		}

		bool bRebootRequired = FileProperties.m_bRebootRequired;
		bool bSecurityPatchesNeeded = nSecurityPatches > 0;

		if (!m_OsPatchStatusReporter)
		{
			CDistributedAppSensorReporter::CSensorInfo SensorInfo;
			SensorInfo.m_Identifier = "org.malterlib.os.patch.status";
			SensorInfo.m_Name = "OS Patch Status";
			SensorInfo.m_Type = CDistributedAppSensorReporter::ESensorDataType_Status;
			if (m_Config.m_PatchInterval != 0.0)
				SensorInfo.m_ExpectedReportInterval = m_Config.m_PatchInterval;

			m_OsPatchStatusReporter = co_await m_SensorStore(&CDistributedAppSensorStoreLocal::f_OpenSensorReporter, fg_Move(SensorInfo));
		}

		bool bUpdateDatabase = false;

		CCloudManager::CVersion CloudManagerVersion;
		CloudManagerVersion.m_Major = m_CurrentOsVersion.m_Major;
		CloudManagerVersion.m_Minor = m_CurrentOsVersion.m_Minor;
		CloudManagerVersion.m_Revision = m_CurrentOsVersion.m_Revision;

		CDistributedAppSensorReporter::CStatus Status;
		Status.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Ok;

		auto fSetProblemStatus = [&](bool _bIsProblem, CStr const &_Description, CTime &o_ProblemStartTime, fp64 const &_ReportWarningAfter, fp64 const &_ReportErrorAfter)
			{
				if (!_bIsProblem)
				{
					if (o_ProblemStartTime.f_IsValid())
					{
						bUpdateDatabase = true;
						o_ProblemStartTime = CTime();
					}

					return;
				}

				if (!o_ProblemStartTime.f_IsValid())
				{
					bUpdateDatabase = true;
					o_ProblemStartTime = CTime::fs_NowUTC();
				}

				auto TimeInProblem = (CTime::fs_NowUTC() - o_ProblemStartTime).f_GetSecondsFraction();

				if (TimeInProblem >= _ReportErrorAfter)
					Status.m_Severity = fg_Max(Status.m_Severity, CDistributedAppSensorReporter::EStatusSeverity_Error);
				else if (TimeInProblem >= _ReportWarningAfter)
					Status.m_Severity = fg_Max(Status.m_Severity, CDistributedAppSensorReporter::EStatusSeverity_Warning);
				else
					Status.m_Severity = fg_Max(Status.m_Severity, CDistributedAppSensorReporter::EStatusSeverity_Info);

				fg_AddStrSep(Status.m_Description, "{} {}"_f << _Description << fg_SecondsDurationToHumanReadable(TimeInProblem), "\n");
			}
		;

		fSetProblemStatus
			(
				bRebootRequired
				, "Reboot has been required for"
				, m_PatchDatabaseState.m_ProblemStart_RebootRequired
				, m_Config.m_ReportWarningAfter_RebootRequired
				, m_Config.m_ReportErrorAfter_RebootRequired
			)
		;

		fSetProblemStatus
			(
				bSecurityPatchesNeeded
				, "Security patches have needed to be installed for"
				, m_PatchDatabaseState.m_ProblemStart_SecurityPatches
				, m_Config.m_ReportWarningAfter_SecurityPatch
				, m_Config.m_ReportErrorAfter_SecurityPatch
			)
		;

		if (nNormalPatches)
			fg_AddStrSep(Status.m_Description, "{} normal patches available to be installed"_f << nNormalPatches, "\n");

		if (nSecurityPatches)
			fg_AddStrSep(Status.m_Description, "{} security patches available to be installed"_f << nSecurityPatches, "\n");

		if (Status.m_Description.f_IsEmpty())
			Status.m_Description = "Patches up to date";

		CDistributedAppSensorReporter::CSensorReading Reading;
		Reading.m_Data = fg_Move(Status);

		co_await m_OsPatchStatusReporter->m_fReportReadings(TCVector<CDistributedAppSensorReporter::CSensorReading>{fg_Move(Reading)}).f_Wrap()
			> LogError("Failed to report readings (OS patch status)")
		;

		co_return bUpdateDatabase;
#endif
	}
}
