// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
		static constexpr CStr c_NeedRestartExecutable = gc_Str<"/usr/sbin/needrestart">;

		auto BlockingActorCheckout = fg_BlockingActor();
		auto [bHasCheckNeedRestart, bReebootRequired] = co_await
			(
				g_Dispatch(BlockingActorCheckout) / [=]() -> TCFuture<TCTuple<bool, bool>>
				{
					auto CaptureExceptions = co_await (g_CaptureExceptions.f_Specific<NFile::CExceptionFile>() % "Failed to read the patch status from file");

					static constexpr CStr c_RebootRequiredFile = gc_Str<"/var/run/reboot-required">;

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

			TCVector<CStr> Arguments = {"-p"};

			// Check only libraries: the kernel check reports UNKN for a kernel managed outside this OS installation
			if (m_Config.m_bKernelManagedExternally)
				Arguments.f_InsertLast("-l");

			CProcessLaunchActor::CSimpleLaunch Launch
				{
					c_NeedRestartExecutable
					, Arguments
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

	auto CHostMonitor::CInternal::f_Patch_PatchesNeeded(bool _bRefreshPackageIndexes) -> TCFuture<CPatchesNeeded>
	{
#ifndef DPlatformFamily_Linux
		co_return {};
#else
		bool bAptCheckExists = false;

		{
			auto BlockingActorCheckout = fg_BlockingActor();
			bAptCheckExists = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [] -> bool
					{
						return CFile::fs_FileExists(gc_AptCheckExecutable);
					}
				)
			;
		}

		CPatchesNeeded PatchesNeeded;

		// Hosts without apt-check, like those of other distributions than Ubuntu, report no pending patches. Where the patches are installed
		// automatically, which configures the refresh, or where the check is required, a missing apt-check would report them as up to date
		if (!bAptCheckExists && (_bRefreshPackageIndexes || m_Config.m_bPatchCheckRequired))
			PatchesNeeded.m_Unavailable = "{} is not installed"_f << gc_AptCheckExecutable;

		if (bAptCheckExists)
		{
			if (_bRefreshPackageIndexes)
			{
				// A refresh fails on the lock of the package lists while another use of the package manager holds it
				CActorSubscription Turn;
				if (m_Config.m_fPackageManagerTurn)
				{
					auto TurnResult = co_await m_Config.m_fPackageManagerTurn().f_Wrap();
					if (TurnResult)
						Turn = fg_Move(*TurnResult);
					else
						DMibLogWithCategory(Malterlib/Cloud/HostMonitor, Error, "Failed to wait for the package manager: {}", TurnResult.f_GetExceptionStr());
				}

				// A repository that fails to refresh only warns unless the error mode is strict, and keeps its old or missing lists
				auto RefreshResult = co_await fg_RunApt("apt-get", {"-o", "APT::Update::Error-Mode=any", "update"}).f_Wrap();

				if (Turn)
					co_await fg_Exchange(Turn, nullptr)->f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/HostMonitor", "Failed to end the turn of the package manager");

				// The old or missing package lists of a failed refresh would report pending patches as none
				if (!RefreshResult)
				{
					PatchesNeeded.m_Unavailable = "Failed to refresh the package indexes: {}"_f << RefreshResult.f_GetExceptionStr();
					co_return fg_Move(PatchesNeeded);
				}
			}

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

		// The reboot check below is local, so it runs also when the pending patches are unknown
		auto fPatchesNeeded = [](TCAsyncResult<CPatchesNeeded> &&_Result) -> CPatchesNeeded
			{
				if (_Result)
					return fg_Move(*_Result);

				CPatchesNeeded PatchesNeeded;
				PatchesNeeded.m_Unavailable = _Result.f_GetExceptionStr();
				return PatchesNeeded;
			}
		;

		auto PatchesNeeded = fPatchesNeeded(co_await f_Patch_PatchesNeeded(m_Config.m_bRefreshPackageIndexes).f_Wrap());

		// The flags choose which pending patches start an installation, which then installs every pending update, security or not, since
		// f_Patch_InstallPatches has no installation of security updates alone
		if
		(
			(PatchesNeeded.m_nSecurityPatches && fg_IsSet(m_Config.m_AutomaticUpdateFlags, EAutomaticUpdatesFlag::mc_SecurityUpdates))
			|| (PatchesNeeded.m_nNormalPatches && fg_IsSet(m_Config.m_AutomaticUpdateFlags, EAutomaticUpdatesFlag::mc_NormalUpdates))
		)
		{
			// A failed start of patching means the installation must not run now
			bool bInstall = true;
			if (m_Config.m_fOnPatchingChanged)
			{
				auto StartResult = co_await m_Config.m_fOnPatchingChanged(true).f_Wrap();
				if (!StartResult)
				{
					DMibLogWithCategory(Malterlib/Cloud/HostMonitor, Warning, "Skipped installing patches automatically: {}", StartResult.f_GetExceptionStr());
					bInstall = false;
				}
			}

			if (bInstall)
			{
				co_await f_Patch_InstallPatches().f_Wrap() > LogError("Failed to install patches automatically (OS patch status)");

				if (m_Config.m_fOnPatchingChanged)
					co_await m_Config.m_fOnPatchingChanged(false).f_Wrap() > LogError("Failed to report patching end (OS patch status)");

				// Update patch state after install
				PatchesNeeded = fPatchesNeeded(co_await f_Patch_PatchesNeeded(false).f_Wrap());
			}
		}

		bool bRebootRequired = co_await f_Patch_RebootNeeded();

		// Unknown pending patches keep the age of the security patches that were pending, which only a check that finds none ends
		bool bSecurityPatchesNeeded = PatchesNeeded.m_Unavailable.f_IsEmpty() ? PatchesNeeded.m_nSecurityPatches > 0 : m_PatchDatabaseState.m_ProblemStart_SecurityPatches.f_IsValid();

		if (!m_OsPatchStatusReporter)
		{
			CDistributedAppSensorReporter::CSensorInfo SensorInfo;
			SensorInfo.m_Identifier = "org.malterlib.os.patch.status";
			SensorInfo.m_Name = "OS Patch Status";
			SensorInfo.m_Type = CDistributedAppSensorReporter::ESensorDataType_Status;
			SensorInfo.m_Metadata = m_SensorMetadata;
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

		if (!PatchesNeeded.m_Unavailable.f_IsEmpty())
		{
			Status.m_Severity = fg_Max(Status.m_Severity, CDistributedAppSensorReporter::EStatusSeverity_Warning);
			fg_AddStrSep(Status.m_Description, "Pending patches unknown: {}"_f << PatchesNeeded.m_Unavailable, "\n");
		}

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
