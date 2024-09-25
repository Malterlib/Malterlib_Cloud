// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

#include <Mib/Concurrency/LogError>
#include <Mib/Process/ProcessLaunchActor>

namespace NMib::NCloud
{
	using namespace NProcess;

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

		CFileProperties FileProperties;
		{
			auto BlockingActorCheckout = fg_BlockingActor();
			FileProperties = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [=]() -> TCFuture<CFileProperties>
					{
						auto CaptureExceptions = co_await (g_CaptureExceptions.f_Specific<NFile::CExceptionFile>() % "Failed to read the patch status from file");

						CStr RebootRequiredFile = "/var/run/reboot-required";

						CFileProperties Return;
						Return.m_bAptCheckExists = CFile::fs_FileExists(AptCheckExecutable);
						Return.m_bRebootRequired = CFile::fs_FileExists(RebootRequiredFile) && !CFile::fs_ReadStringFromFile(RebootRequiredFile, true).f_Trim().f_IsEmpty();

						co_return fg_Move(Return);
					}
				)
			;
		}

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
			SensorInfo.m_MetaData = m_SensorMetaData;
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
