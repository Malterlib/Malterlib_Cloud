// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/LogError>
#include <Mib/Process/ProcessLaunchActor>

namespace NMib::NCloud
{
	using namespace NProcess;

	TCFuture<bool> CHostMonitor::CInternal::f_Patch_RebootNeeded()
	{
#ifndef DPlatformFamily_Linux
		co_return false;
#else
		constexpr static CStr c_NeedRestartExecutable = gc_Str<"/usr/sbin/needrestart">;

		auto BlockingActorCheckout = fg_BlockingActor();
		auto [bHasCheckNeedRestart, bReebootRequired] = co_await
			(
				g_Dispatch(BlockingActorCheckout) / [=]() -> TCFuture<TCTuple<bool, bool>>
				{

					auto CaptureExceptions = co_await (g_CaptureExceptions.f_Specific<NFile::CExceptionFile>() % "Failed to read the patch status from file");

					constexpr static CStr c_RebootRequiredFile = gc_Str<"/var/run/reboot-required">;

					co_return
						{
							CFile::fs_FileExists(c_NeedRestartExecutable)
							, CFile::fs_FileExists(c_RebootRequiredFile) && !CFile::fs_ReadStringFromFile(c_RebootRequiredFile, true).f_Trim().f_IsEmpty()
						}
					;
				}
			)
		;

		if (bHasCheckNeedRestart)
		{
			TCActor<CProcessLaunchActor> LaunchActor(fg_Construct());
			auto AutoDestroy = co_await fg_AsyncDestroy(LaunchActor);

			CProcessLaunchActor::CSimpleLaunch Launch
				{
					c_NeedRestartExecutable
					, {"-p"}
					, CFile::fs_GetPath(c_NeedRestartExecutable)
					, CProcessLaunchActor::ESimpleLaunchFlag_None
				}
			;

			Launch.m_ToLog = CProcessLaunchActor::ELogFlag_None;

			auto LaunchResult = co_await LaunchActor(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch));

			auto Output = LaunchResult.f_GetStdOut();

			if (LaunchResult.m_ExitCode == 0 && Output.f_StartsWith("OK "))
				co_return bReebootRequired;
			else if (LaunchResult.m_ExitCode == 1 && Output.f_StartsWith("WARN "))
				co_return true; // Services etc needs restarting, just reboot instead
			else if (LaunchResult.m_ExitCode == 2 && Output.f_StartsWith("CRIT "))
				co_return true; // Kernel updates needs restart
			else if (LaunchResult.m_ExitCode == 3 && Output.f_StartsWith("UNKN "))
				DMibLogWithCategory(Malterlib/Cloud/HostMonitor, Warning, "needrestart couldn't determine if reboot is needed: {}", LaunchResult.f_GetCombinedOut());
			else
				DMibLogWithCategory(Malterlib/Cloud/HostMonitor, Warning, "needrestart check failed: {}", LaunchResult.f_GetCombinedOut());
		}

		co_return bReebootRequired;
#endif
	}

#ifdef DPlatformFamily_Linux
	static constexpr CStr gc_AptCheckExecutable = gc_Str<"/usr/lib/update-notifier/apt-check">;
#endif

	auto CHostMonitor::CInternal::f_Patch_PatchesNeeded() -> TCFuture<CPatchesNeeded>
	{
#ifndef DPlatformFamily_Linux
		co_return {};
#else
		bool bAptCheckExists = false;

		{
			auto BlockingActorCheckout = fg_BlockingActor();
			bAptCheckExists = co_await
				(
					g_Dispatch(BlockingActorCheckout) / []() -> bool
					{
						return CFile::fs_FileExists(gc_AptCheckExecutable);
					}
				)
			;
		}

		CPatchesNeeded PatchesNeeded;

		if (bAptCheckExists)
		{
			TCActor<CProcessLaunchActor> LaunchActor(fg_Construct());
			auto AutoDestroy = co_await fg_AsyncDestroy(LaunchActor);

			CProcessLaunchActor::CSimpleLaunch Launch
				{
					gc_AptCheckExecutable
					, {}
					, CFile::fs_GetPath(gc_AptCheckExecutable)
					, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
				}
			;

			Launch.m_ToLog = CProcessLaunchActor::ELogFlag_Error;

			auto PatchesNeededStrings = (co_await LaunchActor(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch))).f_GetStdErr().f_Trim().f_Split<true>(";");

			if (PatchesNeededStrings.f_GetLen() >= 1)
				PatchesNeeded.m_nNormalPatches = PatchesNeededStrings[0].f_ToInt(uint32(0));

			if (PatchesNeededStrings.f_GetLen() >= 2)
				PatchesNeeded.m_nSecurityPatches = PatchesNeededStrings[1].f_ToInt(uint32(0));
		}

		co_return PatchesNeeded;
#endif
	}

	TCFuture<bool> CHostMonitor::CInternal::f_PeriodicUpdate_Patch_PatchStatus()
	{
#ifndef DPlatformFamily_Linux
		co_return {};
#else
		auto OnResume = co_await m_pThis->f_CheckDestroyedOnResume();

		CLogError LogError("Malterlib/Cloud/HostMonitor");

		auto PatchesNeeded = co_await f_Patch_PatchesNeeded();

		if
			(
				(PatchesNeeded.m_nSecurityPatches && fg_IsSet(m_Config.m_AutomaticUpdateFlags, EAutomaticUpdatesFlag::mc_SecurityUpdates))
				|| (PatchesNeeded.m_nNormalPatches && fg_IsSet(m_Config.m_AutomaticUpdateFlags, EAutomaticUpdatesFlag::mc_NormalUpdates))
			)
		{
			co_await f_Patch_InstallPatches().f_Wrap() > LogError("Failed to install patches automatically (OS patch status)");

			// Update patch state after install
			PatchesNeeded = co_await f_Patch_PatchesNeeded();
		}

		bool bRebootRequired = co_await f_Patch_RebootNeeded();
		bool bSecurityPatchesNeeded = PatchesNeeded.m_nSecurityPatches > 0;

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

		if (PatchesNeeded.m_nNormalPatches)
			fg_AddStrSep(Status.m_Description, "{} normal patches available to be installed"_f << PatchesNeeded.m_nNormalPatches, "\n");

		if (PatchesNeeded.m_nSecurityPatches)
			fg_AddStrSep(Status.m_Description, "{} security patches available to be installed"_f << PatchesNeeded.m_nSecurityPatches, "\n");

		if (Status.m_Description.f_IsEmpty())
			Status.m_Description = "Patches up to date";

		CDistributedAppSensorReporter::CSensorReading Reading;
		Reading.m_Data = fg_Move(Status);

		co_await m_OsPatchStatusReporter->m_fReportReadings(TCVector<CDistributedAppSensorReporter::CSensorReading>{fg_Move(Reading)}).f_Wrap()
			> LogError("Failed to report readings (OS patch status)")
		;

		if (bRebootRequired && fg_IsSet(m_Config.m_AutomaticUpdateFlags, EAutomaticUpdatesFlag::mc_AutomaticReboot))
		{
			if (m_Config.m_fOnRebootNeeded)
				m_Config.m_fOnRebootNeeded() > LogError("Failed to initiate automatic reboot (OS patch status)");
		}

		co_return bUpdateDatabase;
#endif
	}
}
