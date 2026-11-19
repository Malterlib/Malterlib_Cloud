// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_AppManager.h"

#include <Mib/Concurrency/LogError>

namespace NMib::NCloud
{
	void CAppManagerGuestDeferral::f_Update(bool _bActive, fp64 _Now)
	{
		if (_bActive == m_bActive)
			return;

		m_bActive = _bActive;

		if (!_bActive)
		{
			m_Finished = _Now;
			m_bFinished = true;
			return;
		}

		bool bContinues = m_bFinished && _Now - m_Finished < mc_LimitSeconds;
		if (!bContinues)
			m_Start = _Now;
	}

	bool CAppManagerGuestDeferral::f_Defers(fp64 _Now) const
	{
		return m_bActive && _Now - m_Start < mc_LimitSeconds;
	}

	// The time from which an active deferral no longer defers
	fp64 CAppManagerGuestDeferral::f_End() const
	{
		return m_Start + mc_LimitSeconds;
	}

	fp64 CAppManagerGuestDeferral::fs_Now()
	{
		return fp64(NTime::CSystem_Time::fs_GetTimerValue()) * NTime::CSystem_Time::fs_TimerFrequencyReciprocal();
	}
}

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_RebootPrevention_WatchInitialSensors()
	{
		CDistributedAppSensorReader_SensorFilter Filter;
		Filter.m_Flags = CDistributedAppSensorReader_SensorFilter::ESensorFlag_IgnoreRemoved;

		auto SensorsGenerator = co_await mp_SensorStore(&CDistributedAppSensorStoreLocal::f_GetSensors, CDistributedAppSensorReader::CGetSensors{.m_Filters = {Filter}});

		for (auto iSensors = co_await fg_Move(SensorsGenerator).f_GetPipelinedIterator(); iSensors; co_await ++iSensors)
		{
			for (auto &SensorInfo : *iSensors)
			{
				if (!SensorInfo.m_Scope.f_IsOfType<CDistributedAppSensorReporter::CSensorScope_Application>())
					continue;

				auto &Application = SensorInfo.m_Scope.f_GetAsType<CDistributedAppSensorReporter::CSensorScope_Application>();

				auto *pApplication = mp_Applications.f_FindEqual(Application.m_ApplicationName);
				if (pApplication)
					co_await fp_RebootPrevention_WatchSensor(*pApplication, SensorInfo);
			}
		}

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_RebootPrevention_RemoveApplication(CStr _Application)
	{
		TCSet<CDistributedAppSensorReporter::CSensorInfoKey> ToRemove;
		TCFutureVector<void> RemoveResults;

		if (auto *pKeys = mp_SensorRebootPreventionWatchByApplication.f_FindEqual(_Application))
			ToRemove = *pKeys;

		for (auto &SensorWatchKey : ToRemove)
		{
			auto *pSensorWatch = mp_SensorRebootPreventionWatch.f_FindEqual(SensorWatchKey);
			if (pSensorWatch && pSensorWatch->m_SensorSubscription)
				fg_Exchange(pSensorWatch->m_SensorSubscription, nullptr)->f_Destroy() > RemoveResults;

			fp_RebootPrevention_RemoveWatch(SensorWatchKey);
		}

		co_await fg_AllDone(RemoveResults).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to remove sensor subscription");

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_RebootPrevention_UpdateApplications()
	{
		auto Now = CTime::fs_NowUTC();

		for (auto &Watch : mp_SensorRebootPreventionWatch)
			fp_RebootPrevention_UpdateWatchFlags(mp_SensorRebootPreventionWatch.fs_GetKey(Watch), Watch, Now);

		for (auto &Keys : mp_SensorRebootPreventionWatchByApplication)
			fp_RebootPrevention_UpdateApplicationFlags(mp_SensorRebootPreventionWatchByApplication.fs_GetKey(Keys));

		co_return {};
	}

	void CAppManagerActor::fp_RebootPrevention_RemoveWatch(CDistributedAppSensorReporter::CSensorInfoKey const &_Key)
	{
		auto *pWatch = mp_SensorRebootPreventionWatch.f_FindEqual(_Key);
		if (!pWatch)
			return;

		fp_RebootPrevention_SetWatchFlags(_Key, *pWatch, CDistributedAppSensorReporter::ESensorInfoFlag::mc_None);

		if (auto *pKeys = mp_SensorRebootPreventionWatchByApplication.f_FindEqual(pWatch->m_Application))
		{
			pKeys->f_Remove(_Key);
			if (pKeys->f_IsEmpty())
				mp_SensorRebootPreventionWatchByApplication.f_Remove(pWatch->m_Application);
		}

		mp_SensorRebootPreventionWatch.f_Remove(_Key);
	}

	// An application prevents reboots while any of its sensors does. Only the sensors that the host it is associated with reports count: an
	// application added again with the same name, like one that ran in a guest before, can have sensors of its previous reporter in the sensor
	// store, whose readings no longer change. A reading updates its application from the counts of mp_SensorRebootPreventionCounts, without
	// scanning the other watches, since a guest can register any number
	void CAppManagerActor::fp_RebootPrevention_UpdateApplicationFlags(CStr const &_Application)
	{
		auto pApplication = mp_Applications.f_FindEqual(_Application);
		if (!pApplication)
			return;

		auto &Application = **pApplication;

		CDistributedAppSensorReporter::ESensorInfoFlag Prevented = CDistributedAppSensorReporter::ESensorInfoFlag::mc_None;
		if (auto *pHosts = mp_SensorRebootPreventionCounts.f_FindEqual(_Application))
		{
			if (auto *pCounts = pHosts->f_FindEqual(Application.m_AssociatedHostID))
			{
				if (pCounts->m_nPreventRebootOnWarning)
					Prevented |= CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnWarning;
				if (pCounts->m_nPreventRebootOnError)
					Prevented |= CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnError;
				if (pCounts->m_nPreventRebootOnOutdated)
					Prevented |= CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnOutdated;
			}
		}

		fp_RebootPrevention_SetApplicationFlags(Application, Prevented);
	}

	// Every change of the flags of a watch goes through here, which keeps mp_SensorRebootPreventionCounts in step
	void CAppManagerActor::fp_RebootPrevention_SetWatchFlags
		(
			CDistributedAppSensorReporter::CSensorInfoKey const &_Key
			, CSensorRebootWatch &_Watch
			, CDistributedAppSensorReporter::ESensorInfoFlag _Prevented
		)
	{
		if (_Watch.m_PreventedFlags == _Prevented)
			return;

		auto &Hosts = mp_SensorRebootPreventionCounts[_Watch.m_Application];
		auto &Counts = Hosts[_Key.m_HostID];

		auto fUpdateCount = [&](umint &o_nCount, CDistributedAppSensorReporter::ESensorInfoFlag _Flag)
			{
				bool bWasSet = fg_IsSet(_Watch.m_PreventedFlags, _Flag);
				bool bIsSet = fg_IsSet(_Prevented, _Flag);
				if (bIsSet && !bWasSet)
					++o_nCount;
				else if (bWasSet && !bIsSet)
					--o_nCount;
			}
		;

		fUpdateCount(Counts.m_nPreventRebootOnWarning, CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnWarning);
		fUpdateCount(Counts.m_nPreventRebootOnError, CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnError);
		fUpdateCount(Counts.m_nPreventRebootOnOutdated, CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnOutdated);

		_Watch.m_PreventedFlags = _Prevented;

		if (!Counts.m_nPreventRebootOnWarning && !Counts.m_nPreventRebootOnError && !Counts.m_nPreventRebootOnOutdated)
		{
			Hosts.f_Remove(_Key.m_HostID);
			if (Hosts.f_IsEmpty())
				mp_SensorRebootPreventionCounts.f_Remove(_Watch.m_Application);
		}
	}

	void CAppManagerActor::fp_RebootPrevention_SetApplicationFlags(CApplication &_Application, CDistributedAppSensorReporter::ESensorInfoFlag _Prevented)
	{
		_Application.m_PreventRebootSensorFlags = _Prevented;
		_Application.m_PreventRebootDeferral.f_Update(fg_IsSet(_Prevented, CDistributedAppSensorReporter::ESensorInfoFlag::mc_AllPreventRebootFlags), CAppManagerGuestDeferral::fs_Now());
	}

	void CAppManagerActor::fp_RebootPrevention_UpdateWatchFlags(CDistributedAppSensorReporter::CSensorInfoKey const &_Key, CSensorRebootWatch &_Watch, NTime::CTime const &_Now)
	{
		if (!_Watch.m_LastReading)
			return;

		auto &Reading = *_Watch.m_LastReading;
		auto &SensorInfo = _Watch.m_SensorInfo;

		auto Flags = SensorInfo.m_Flags;
		CDistributedAppSensorReporter::ESensorInfoFlag Prevented = CDistributedAppSensorReporter::ESensorInfoFlag::mc_None;

		auto DataStatus = Reading.m_Reading.f_GetDataStatus(&SensorInfo);

		if (fg_IsSet(Flags, CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnWarning))
		{
			if (DataStatus >= CDistributedAppSensorReporter::EStatusSeverity_Warning)
				Prevented |= CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnWarning;
			else if (fg_IsSet(Flags, CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnOutdated))
			{
				fp64 OutdatedSeconds;
				if (Reading.m_Reading.f_OutdatedStatus(SensorInfo, _Now, OutdatedSeconds) >= CDistributedAppSensorReporter::EStatusSeverity_Warning)
					Prevented |= CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnWarning | CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnOutdated;
			}
		}

		if (fg_IsSet(Flags, CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnError))
		{
			if (DataStatus >= CDistributedAppSensorReporter::EStatusSeverity_Error)
				Prevented |= CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnError;
			else if (fg_IsSet(Flags, CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnOutdated))
			{
				fp64 OutdatedSeconds;
				if (Reading.m_Reading.f_OutdatedStatus(SensorInfo, _Now, OutdatedSeconds) >= CDistributedAppSensorReporter::EStatusSeverity_Error)
					Prevented |= CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnError | CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnOutdated;
			}
		}

		fp_RebootPrevention_SetWatchFlags(_Key, _Watch, Prevented);
	}

	bool CAppManagerActor::fp_ApplicationRunsInGuest(CApplication const &_Application)
	{
		auto *pEnvironment = mp_Environments.f_FindEqual(_Application.m_Settings.m_LaunchEnvironment);
		return pEnvironment && (*pEnvironment)->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Local;
	}

	// Whether a stop of the application waits only for container and VM guests: the application runs in one, and the stops of its dependents and
	// children that its stop waits for first wait only for guests as well
	bool CAppManagerActor::fp_StopWaitsOnlyForGuests(CApplication const &_Application)
	{
		TCSet<CApplication const *> Visited;
		return fp_StopWaitsOnlyForGuests(_Application, Visited);
	}

	// The settings allow dependency cycles, and an application that o_Visited has is already checked
	bool CAppManagerActor::fp_StopWaitsOnlyForGuests(CApplication const &_Application, TCSet<CApplication const *> &o_Visited)
	{
		if (!o_Visited(&_Application).f_WasCreated())
			return true;

		if (!fp_ApplicationRunsInGuest(_Application))
			return false;

		for (auto &pDependent : _Application.f_GetDependents())
		{
			if (!fp_StopWaitsOnlyForGuests(*pDependent, o_Visited))
				return false;
		}

		for (auto &Child : _Application.m_Children)
		{
			if (!fp_StopWaitsOnlyForGuests(Child, o_Visited))
				return false;
		}

		return true;
	}

	// The sensors of an application that runs in a container or VM environment are reported from the guest, so they prevent host reboots and restarts
	// of the environment only within the limit of CAppManagerGuestDeferral. _Now is from CAppManagerGuestDeferral::fs_Now
	bool CAppManagerActor::fp_ApplicationPreventsReboot(CApplication const &_Application, fp64 _Now)
	{
		if (!fg_IsSet(_Application.m_PreventRebootSensorFlags, CDistributedAppSensorReporter::ESensorInfoFlag::mc_AllPreventRebootFlags))
			return false;

		if (!fp_ApplicationRunsInGuest(_Application))
			return true;

		return _Application.m_PreventRebootDeferral.f_Defers(_Now);
	}

	TCFuture<void> CAppManagerActor::fp_RebootPrevention_WatchSensor(TCSharedPointer<CApplication> _pApplication, CDistributedAppSensorReporter::CSensorInfo _SensorInfo)
	{
		auto CheckDestory = co_await f_CheckDestroyedOnResume();

		// A watch of the same sensor that starts while this one waits would be erased or replaced when this one resumes
		auto Sequence = co_await mp_SensorRebootPreventionWatchSequencer.f_Sequence();

		CStr Application = _pApplication->m_Name;

		static auto constexpr c_RebootPreventionFlags
			= CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnWarning
			| CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnError
			| CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnOutdated
		;

		auto Key = _SensorInfo.f_Key();

		auto pPrevention = mp_SensorRebootPreventionWatch.f_FindEqual(Key);
		if (pPrevention)
		{
			DMibFastCheck(pPrevention->m_Application == Application);

			if (pPrevention->m_SensorSubscription)
				co_await fg_Exchange(pPrevention->m_SensorSubscription, nullptr)->f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to remove sensor subscription");
		}

		bool bDoWatch = fg_IsSet(_SensorInfo.m_Flags, c_RebootPreventionFlags);

		// A sensor that no longer prevents reboots stops counting for its application
		if (!bDoWatch)
		{
			if (mp_SensorRebootPreventionWatch.f_FindEqual(Key))
			{
				fp_RebootPrevention_RemoveWatch(Key);
				fp_RebootPrevention_UpdateApplicationFlags(Application);
			}

			co_return {};
		}

		CDistributedAppSensorReader_SensorStatusFilter Filter;
		Filter.m_SensorFilter.m_HostID = Key.m_HostID;
		Filter.m_SensorFilter.m_Scope = Key.m_Scope;
		Filter.m_SensorFilter.m_Identifier = Key.m_Identifier;
		Filter.m_SensorFilter.m_IdentifierScope = Key.m_IdentifierScope;

		if (fg_IsSet(_SensorInfo.m_Flags, CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnOutdated))
		{
			if (!mp_SensorRebootPreventionTimerSubscription)
			{
				mp_SensorRebootPreventionTimerSubscription = co_await fg_RegisterTimer
					(
						60.0
						, [this]() -> TCFuture<void>
						{
							co_await fp_RebootPrevention_UpdateApplications();

							co_return {};
						}
					)
				;
			}
		}

		// The removal of the application, which can run while this one waits, removes only the watches that are already there
		if (_pApplication->m_bDeleted)
			co_return {};

		auto &Watch = mp_SensorRebootPreventionWatch[Key];
		Watch.m_Application = Application;
		mp_SensorRebootPreventionWatchByApplication[Application][Key];
		Watch.m_SensorInfo = _SensorInfo;
		auto SensorSubscription = co_await mp_SensorStore
			(
				&CDistributedAppSensorStoreLocal::f_SubscribeSensorStatus
				, TCVector<CDistributedAppSensorReader_SensorStatusFilter>{fg_Move(Filter)}
				, g_ActorFunctor / [this](CDistributedAppSensorReader_SensorKeyAndReading _Reading) -> TCFuture<void>
				{
					auto *pWatch = mp_SensorRebootPreventionWatch.f_FindEqual(_Reading.m_SensorInfoKey);
					if (!pWatch)
						co_return {};

					pWatch->m_LastReading = fg_Move(_Reading);
					fp_RebootPrevention_UpdateWatchFlags(mp_SensorRebootPreventionWatch.fs_GetKey(*pWatch), *pWatch, CTime::fs_NowUTC());
					fp_RebootPrevention_UpdateApplicationFlags(pWatch->m_Application);

					co_return {};
				}
			)
		;

		// The watch can have been removed while subscribing
		auto *pWatch = mp_SensorRebootPreventionWatch.f_FindEqual(Key);
		if (!pWatch)
		{
			if (SensorSubscription)
				co_await SensorSubscription->f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to remove sensor subscription");

			co_return {};
		}

		pWatch->m_SensorSubscription = fg_Move(SensorSubscription);

		co_return {};
	}

	// The guests of container and VM environments are left out with m_bIgnoreGuestDeferrals
	auto CAppManagerActor::fp_CheckAndLogPreventedReboot(CCheckAndLogPreventedRebootParams _Params) -> TCFuture<EPreventedReboot>
	{
		CStr PreventRebootDescription;
		bool bPreventReboot = false;
		bool bHostPreventsReboot = false;
		fp64 Now = CAppManagerGuestDeferral::fs_Now();

		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;

			bool bGuest = fp_ApplicationRunsInGuest(Application);

			// An operation that only waits for the guests of container or VM environments is a guest deferral, also one of a host application that
			// waits for the stops of its dependents in guests
			if (Application.f_IsInProgress())
			{
				bool bGuestWait = Application.f_InProgressWaitsOnlyForGuest();
				if (!(bGuestWait && _Params.m_bIgnoreGuestDeferrals))
				{
					fg_AddStrSep(PreventRebootDescription, "    Operation in progress ({}): {}"_f << Application.m_Name << Application.f_InProgressDescription(), "\n");
					bPreventReboot = true;
					if (!bGuestWait)
						bHostPreventsReboot = true;
				}
			}

			if (fp_ApplicationPreventsReboot(Application, Now) && !(bGuest && _Params.m_bIgnoreGuestDeferrals))
			{
				fg_AddStrSep
					(
						PreventRebootDescription
						, "    Sensor state ({}): {vs}"_f << Application.m_Name << CDistributedAppSensorReporter::fs_FlagsToStringArray(Application.m_PreventRebootSensorFlags)
						, "\n"
					)
				;
				bPreventReboot = true;
				if (!bGuest)
					bHostPreventsReboot = true;
			}
		}

		if (mp_nEnvironmentOSDependencyInstalls)
		{
			fg_AddStrSep(PreventRebootDescription, "    OS dependency installations in progress: {}"_f << mp_nEnvironmentOSDependencyInstalls, "\n");
			bPreventReboot = true;
			bHostPreventsReboot = true;
		}

		// A reboot would interrupt updates the environment agents are installing
		for (auto &pEnvironment : mp_Environments)
		{
			auto &Environment = *pEnvironment;
			if (!Environment.f_AgentUpdateDefers())
				continue;

			bool bGuest = Environment.m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Local;
			if (bGuest && _Params.m_bIgnoreGuestDeferrals)
				continue;

			fg_AddStrSep(PreventRebootDescription, "    Environment update in progress ({}): {}"_f << Environment.m_Name << Environment.m_AgentUpdateInProgress, "\n");
			bPreventReboot = true;
			if (!bGuest)
				bHostPreventsReboot = true;
		}

		if (PreventRebootDescription != mp_LastPreventRebootDescription)
		{
			if (bPreventReboot)
			{
				if (_Params.m_bCriticalLog)
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Critical, "Prevented reboot:\n{}", PreventRebootDescription);
				else
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Prevented reboot:\n{}", PreventRebootDescription);
			}
			mp_LastPreventRebootDescription = PreventRebootDescription;
		}

		if (bPreventReboot && _Params.m_bErrorOnPreventReboot)
			co_return DMibErrorInstance("Prevented reboot:\n{}"_f << PreventRebootDescription);

		if (bHostPreventsReboot)
			co_return EPreventedReboot::mc_Host;

		if (bPreventReboot)
			co_return EPreventedReboot::mc_GuestDeferrals;

		co_return EPreventedReboot::mc_None;
	}
}
