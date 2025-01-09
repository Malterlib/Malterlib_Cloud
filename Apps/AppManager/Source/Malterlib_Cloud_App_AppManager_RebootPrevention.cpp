// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

#include <Mib/Concurrency/LogError>

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
					co_await fp_RebootPrevention_WatchSensor(Application.m_ApplicationName, SensorInfo);
			}
		}

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_RebootPrevention_RemoveApplication(CStr _Application)
	{
		TCSet<CDistributedAppSensorReporter::CSensorInfoKey> ToRemove;
		TCFutureVector<void> RemoveResults;

		for (auto &SensorWatchEntry : mp_SensorRebootPreventionWatch.f_Entries())
		{
			auto &SensorWatch = SensorWatchEntry.f_Value();
			auto &SensorWatchKey = SensorWatchEntry.f_Key();

			DMibFastCheck(SensorWatchKey.m_Scope.f_IsOfType<CDistributedAppSensorReporter::CSensorScope_Application>());
			if (!SensorWatchKey.m_Scope.f_IsOfType<CDistributedAppSensorReporter::CSensorScope_Application>())
				continue;
			if (SensorWatchKey.m_Scope.f_GetAsType<CDistributedAppSensorReporter::CSensorScope_Application>().m_ApplicationName != _Application)
				continue;

			ToRemove[SensorWatchKey];

			if (SensorWatch.m_SensorSubscription)
				fg_Exchange(SensorWatch.m_SensorSubscription, nullptr)->f_Destroy() > RemoveResults;
		}

		for (auto &SensorWatchKey : ToRemove)
			mp_SensorRebootPreventionWatch.f_Remove(SensorWatchKey);

		co_await fg_AllDone(RemoveResults).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to remove sensor subscription");

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_RebootPrevention_UpdateApplications()
	{
		auto Now = CTime::fs_NowUTC();

		for (auto &Watch : mp_SensorRebootPreventionWatch)
			fp_RebootPrevention_UpdateApplicationFlags(Watch, Now);

		co_return {};
	}

	void CAppManagerActor::fp_RebootPrevention_UpdateApplicationFlags(CSensorRebootWatch const &_Watch, NTime::CTime const &_Now)
	{
		auto pApplication = mp_Applications.f_FindEqual(_Watch.m_Application);
		if (!pApplication || !_Watch.m_LastReading)
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

		(*pApplication)->m_PreventRebootSensorFlags = Prevented;
	}

	TCFuture<void> CAppManagerActor::fp_RebootPrevention_WatchSensor(CStr _Application, CDistributedAppSensorReporter::CSensorInfo _SensorInfo)
	{
		auto CheckDestory = co_await f_CheckDestroyedOnResume();

		static auto constexpr c_RebootPreventionFlags = CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnWarning
			| CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnError
			| CDistributedAppSensorReporter::ESensorInfoFlag::mc_PreventRebootOnOutdated
		;

		auto Key = _SensorInfo.f_Key();

		auto pPrevention = mp_SensorRebootPreventionWatch.f_FindEqual(Key);
		if (pPrevention)
		{
			DMibFastCheck(pPrevention->m_Application == _Application);

			if (pPrevention->m_SensorSubscription)
			{
				co_await fg_Exchange(pPrevention->m_SensorSubscription, nullptr)->f_Destroy().f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to remove sensor subscription")
				;
			}
		}

		bool bDoWatch = fg_IsSet(_SensorInfo.m_Flags, c_RebootPreventionFlags);

		if (!bDoWatch)
			co_return {};

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

		auto &Watch = mp_SensorRebootPreventionWatch[Key];
		Watch.m_Application = _Application;
		Watch.m_SensorInfo = _SensorInfo;
		Watch.m_SensorSubscription = co_await mp_SensorStore
			(
				&CDistributedAppSensorStoreLocal::f_SubscribeSensorStatus
				, TCVector<CDistributedAppSensorReader_SensorStatusFilter>{fg_Move(Filter)}
				, g_ActorFunctor / [this](CDistributedAppSensorReader_SensorKeyAndReading _Reading) -> TCFuture<void>
				{
					auto *pWatch = mp_SensorRebootPreventionWatch.f_FindEqual(_Reading.m_SensorInfoKey);
					if (!pWatch)
						co_return {};

					pWatch->m_LastReading = fg_Move(_Reading);
					fp_RebootPrevention_UpdateApplicationFlags(*pWatch, CTime::fs_NowUTC());

					co_return {};
				}
			)
		;

		co_return {};
	}

	TCFuture<bool> CAppManagerActor::fp_CheckAndLogPreventedReboot(CCheckAndLogPreventedRebootParams _Params)
	{
		CStr PreventRebootDescription;
		bool bPreventReboot = false;

		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;

			if (Application.f_IsInProgress())
			{
				fg_AddStrSep
					(
						PreventRebootDescription
						, "    Operation in progress ({}): {}"_f << Application.m_Name << Application.f_InProgressDescription()
						, "\n"
					)
				;
				bPreventReboot = true;
			}

			if (fg_IsSet(Application.m_PreventRebootSensorFlags, CDistributedAppSensorReporter::ESensorInfoFlag::mc_AllPreventRebootFlags))
			{
				fg_AddStrSep
					(
						PreventRebootDescription
						, "    Sensor state ({}): {vs}"_f << Application.m_Name << CDistributedAppSensorReporter::fs_FlagsToStringArray(Application.m_PreventRebootSensorFlags)
						, "\n"
					)
				;
				bPreventReboot = true;
			}
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

		co_return bPreventReboot;
	}
}
