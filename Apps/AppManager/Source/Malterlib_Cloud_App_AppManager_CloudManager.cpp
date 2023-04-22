// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_CloudManagerAdded(TCDistributedActor<CCloudManager> const &_CloudManager, CTrustedActorInfo const &_Info)
	{
		if (f_IsDestroyed() || mp_State.m_bStoppingApp)
			co_return DMibErrorInstance("Shutting down");

		CCloudManager::CAppManagerInfo Info;
		Info.m_Environment = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("Environment", "").f_String();
		Info.m_HostName = NProcess::NPlatform::fg_Process_GetFullyQualiedHostName();
		Info.m_Platform = DMibStringize(DPlatform);
		Info.m_PlatformFamily = DMibStringize(DPlatformFamily);
		Info.m_Version = (*g_CloudVersion).m_Version;
		Info.m_ProgramDirectory = mp_Settings.m_RootDirectory;

		NMib::NProcess::CVersionInfo VersionInfo;
		NProcess::NPlatform::fg_Process_GetVersionInfo(CFile::fs_GetProgramPath(), VersionInfo);
		Info.m_VersionDate = VersionInfo.m_BuildTime;

		auto Subscription = co_await _CloudManager.f_CallActor(&CCloudManager::f_RegisterAppManager)
			(
				TCDistributedActorInterfaceWithID<CAppManagerInterface>
				{
					mp_AppManagerInterface.m_Actor->f_ShareInterface<CAppManagerInterface>()
					, g_ActorSubscription / [this, WeakCloudManager = _CloudManager.f_Weak()]() -> TCFuture<void>
					{
						co_await self(&CAppManagerActor::fp_CloudManagerRemoved, WeakCloudManager).f_Wrap()
							> fg_LogWarning("Malterlib/Cloud/AppManager", "Failed to destroy register subscription for cloud manager")
						;

						co_return {};
					}
				}
				, fg_Move(Info)
			)
		;

		CActorSubscription SensorReporterSubscription;
		{
			auto SensorReporter = co_await _CloudManager.f_CallActor(&CCloudManager::f_GetSensorReporter)().f_Wrap();

			if (SensorReporter)
			{
				auto SubscriptionResult = co_await mp_SensorStore
					(
						&CDistributedAppSensorStoreLocal::f_AddExtraSensorReporter
						, fg_Move(*SensorReporter)
						, _Info
					)
					.f_Wrap()
				;

				if (SubscriptionResult)
					SensorReporterSubscription = fg_Move(*SubscriptionResult);
				else
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to add extra sensor reporter to sensor store: {}", SubscriptionResult.f_GetExceptionStr());
			}
			else
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to get sensor reporter from cloud manager: {}", SensorReporter.f_GetExceptionStr());
		}

		CActorSubscription LogReporterSubscription;
		if (_CloudManager->f_InterfaceVersion() >= ECloudManagerProtocolVersion_SupportLogs)
		{
			auto LogReporter = co_await _CloudManager.f_CallActor(&CCloudManager::f_GetLogReporter)().f_Wrap();

			if (LogReporter)
			{
				auto SubscriptionResult = co_await mp_LogStore
					(
						&CDistributedAppLogStoreLocal::f_AddExtraLogReporter
						, fg_Move(*LogReporter)
						, _Info
					)
					.f_Wrap()
				;

				if (SubscriptionResult)
					LogReporterSubscription = fg_Move(*SubscriptionResult);
				else
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to add extra log reporter to sensor store: {}", SubscriptionResult.f_GetExceptionStr());
			}
			else
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to get log reporter from cloud manager: {}", LogReporter.f_GetExceptionStr());
		}

		auto &NewManager = mp_CloudManagers[_CloudManager];
		NewManager.m_HostInfo = _Info;
		NewManager.m_RegisterSubscription = fg_Move(Subscription);
		NewManager.m_SensorReporterSubscription = fg_Move(SensorReporterSubscription);
		NewManager.m_LogReporterSubscription = fg_Move(LogReporterSubscription);

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_CloudManagerRemoved(TCWeakDistributedActor<CActor> const &_CloudManager)
	{
		auto pCloudManagerState = mp_CloudManagers.f_FindEqual(_CloudManager);
		if (!pCloudManagerState)
			co_return {};

		TCActorResultVector<void> DestroyResults;

		if (pCloudManagerState->m_RegisterSubscription)
			fg_Exchange(pCloudManagerState->m_RegisterSubscription, nullptr)->f_Destroy() > DestroyResults.f_AddResult();

		if (pCloudManagerState->m_SensorReporterSubscription)
			fg_Exchange(pCloudManagerState->m_SensorReporterSubscription, nullptr)->f_Destroy() > DestroyResults.f_AddResult();

		if (pCloudManagerState->m_LogReporterSubscription)
			fg_Exchange(pCloudManagerState->m_LogReporterSubscription, nullptr)->f_Destroy() > DestroyResults.f_AddResult();

		mp_CloudManagers.f_Remove(_CloudManager);

		co_await DestroyResults.f_GetUnwrappedResults();

		co_return {};
	}
}
