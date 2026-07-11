// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_AppManager.h"

#include <Mib/Encoding/JsonShortcuts>

namespace NMib::NCloud::NAppManager
{
	namespace
	{
		CHostMonitorRebootSchedule fg_ParseRebootSchedule(NEncoding::CEJsonSorted const &_AutoUpdateConfig)
		{
			CEJsonSorted DefaultSchedule =
				{
					"Hour"_= 0
					, "Minute"_= 0
				}
			;
			auto RebootScheduleJson = _AutoUpdateConfig.f_GetMemberValue("RebootSchedule", DefaultSchedule).f_Object(); // Default to midnight every day

			CHostMonitorRebootSchedule RebootSchedule;

			if (auto *pValue = RebootScheduleJson.f_GetMember("Month", EJsonType_Integer))
				RebootSchedule.m_Month = fg_Clamp(pValue->f_Integer(), 1, 12);

			if (auto *pValue = RebootScheduleJson.f_GetMember("DayOfMonth", EJsonType_Integer))
			{
				if (RebootSchedule.m_Month)
				{
					auto AllowedDaysInMonth = CTimeConvert::fs_GetDaysInMonth(*RebootSchedule.m_Month - 1);
					if (*RebootSchedule.m_Month == 2)
						AllowedDaysInMonth = 29;

					RebootSchedule.m_DayOfMonth = fg_Clamp(pValue->f_Integer(), 1, AllowedDaysInMonth);
				}
				else
					RebootSchedule.m_DayOfMonth = fg_Clamp(pValue->f_Integer(), 1, 31);
			}
			else if (auto *pValue = RebootScheduleJson.f_GetMember("DayOfWeek", EJsonType_Integer))
				RebootSchedule.m_DayOfWeek = fg_Clamp(pValue->f_Integer(), 0, 6);

			if (auto *pValue = RebootScheduleJson.f_GetMember("Hour", EJsonType_Integer))
				RebootSchedule.m_Hour = fg_Clamp(pValue->f_Integer(), 0, 23);

			if (auto *pValue = RebootScheduleJson.f_GetMember("Minute", EJsonType_Integer))
				RebootSchedule.m_Minute = fg_Clamp(pValue->f_Integer(), 0, 59);

			return RebootSchedule;
		}
	}

	NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> ()> CAppManagerActor::fp_HostMonitorRebootNeededFunctor(NEncoding::CEJsonSorted const &_AutoUpdateConfig)
	{
		return g_ActorFunctor / [this, RebootSchedule = fg_ParseRebootSchedule(_AutoUpdateConfig), Generation = mp_HostMonitorGeneration]() -> TCFuture<void>
			{
				if (mp_bRebootScheduled || Generation != mp_HostMonitorGeneration)
					co_return {};

				mp_bRebootScheduled = true;

				auto EarliestRebootTime = CTime::fs_NowUTC();

				if (auto *pValue = mp_State.m_StateDatabase.m_Data.f_GetMember("LastHostMonitorReboot", EEJsonType_Date))
				{
					// Make sure we don't go into a reboot loop
					if ((EarliestRebootTime - pValue->f_Date()).f_GetSecondsFraction() < 24_hours)
						EarliestRebootTime += CTimeSpanConvert::fs_CreateHourSpan(24);
				}

				auto RebootTime = RebootSchedule.f_GetNextRebootTime(EarliestRebootTime);

				fp64 RebootInSeconds = (RebootTime - CTime::fs_NowUTC()).f_GetSecondsFraction();

				if (RebootInSeconds >= 0.001)
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Reboot scheduled for {tc5}", RebootTime);

					auto TimeoutAbortable = co_await fg_TimeoutAbortable(RebootInSeconds);

					// The replacement of the host monitor cancels the timer it finds and schedules again under the new configuration, so the timer of
					// a replaced monitor is aborted here and the wait below fails
					if (Generation != mp_HostMonitorGeneration)
						co_await TimeoutAbortable.m_Subscription->f_Destroy();
					else
						mp_RebootScheduleTimerSubscrption = fg_Move(TimeoutAbortable.m_Subscription);

					co_await fg_Move(TimeoutAbortable.m_Future);
				}
				else
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Rebooting at once");

				// A host monitor that replaces this one while the restart is requested schedules restarts under its own configuration, so this one
				// gives up, also without clearing the scheduled flag of the new monitor
				auto OnResume = co_await fg_OnResume
					(
						[&]() -> CExceptionPointer
						{
							if (Generation != mp_HostMonitorGeneration)
								return DMibErrorInstance("The host monitor was replaced");

							return {};
						}
					)
				;

				// The environment host coordinates the restart with the operations on the environment's applications. The host can fail or abort a
				// restart it accepted, so the restart counts as done only when the agent starts again
				if (mp_bEnvironmentAgent)
				{
					mp_State.m_StateDatabase.m_Data["PendingHostMonitorReboot"] = CTime::fs_NowUTC();
					if (auto SaveResult = co_await mp_State.m_StateDatabase.f_Save().f_Wrap(); !SaveResult)
					{
						// A later patch check requests the restart again
						mp_bRebootScheduled = false;

						co_return DMibErrorInstance("Failed to save state database: {}"_f << SaveResult.f_GetExceptionStr());
					}

					auto Result = co_await fp_RequestEnvironmentRestartFromHost().f_Wrap();

					// The host can still abort an accepted restart, and the restart ends this agent, so a later patch cycle
					// can request it again
					mp_bRebootScheduled = false;

					if (!Result)
						co_return Result.f_GetException();
				}
				else
				{
					mp_State.m_StateDatabase.m_Data["LastHostMonitorReboot"] = CTime::fs_NowUTC();
					co_await (mp_State.m_StateDatabase.f_Save() % "Failed to save state database");

					co_await fp_Reboot(false);
				}

				co_return {};
			}
		;
	}
}
