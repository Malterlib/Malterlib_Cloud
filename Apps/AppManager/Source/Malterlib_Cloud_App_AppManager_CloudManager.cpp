// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_CloudManagerAdded(TCDistributedActor<CCloudManager> _CloudManager, CTrustedActorInfo _Info)
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

		try
		{
			NMib::NProcess::CVersionInfo VersionInfo;
			NProcess::NPlatform::fg_Process_GetVersionInfo(CFile::fs_GetProgramPathForExecutabelContents(), VersionInfo);
			Info.m_VersionDate = VersionInfo.m_BuildTime;
		}
		catch (NException::CException const &_Exception)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to get current executable version: {}", _Exception);
		}

		auto RegisterAppManagerResult = co_await _CloudManager.f_CallActor(&CCloudManager::f_RegisterAppManager)
			(
				TCDistributedActorInterfaceWithID<CAppManagerInterface>
				{
					mp_AppManagerInterface.m_Actor->f_ShareInterface<CAppManagerInterface>()
					, g_ActorSubscription / [this, WeakCloudManager = _CloudManager.f_Weak()]() -> TCFuture<void>
					{
						co_await fp_CloudManagerRemoved(WeakCloudManager).f_Wrap()
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

		auto *pNewManager = &mp_CloudManagers[_CloudManager];
		pNewManager->m_HostInfo = _Info;
		pNewManager->m_AppManagerCloudManagerInterface = fg_Move(RegisterAppManagerResult.m_AppManagerCloudManagerInterface);
		pNewManager->m_SensorReporterSubscription = fg_Move(SensorReporterSubscription);
		pNewManager->m_LogReporterSubscription = fg_Move(LogReporterSubscription);

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					pNewManager = mp_CloudManagers.f_FindEqual(_CloudManager);
					if (!pNewManager)
						return DMibErrorInstance("Cloud manager removed");

					return {};
				}
			)
		;

		CActorSubscription ExpectedOsVersionSubscription;
		if (_CloudManager->f_InterfaceVersion() >= ECloudManagerProtocolVersion_SupportExpectedOsVersions)
		{
			CCloudManager::CSubscribeExpectedOsVersions SubscribeParams;
			SubscribeParams.m_OsName = mp_OsName;
			SubscribeParams.m_fVersionRangeChanged = g_ActorFunctor / [this, CloudManagerWeak = _CloudManager.f_Weak()](CCloudManager::CExpectedVersions _Versions) -> TCFuture<void>
				{
					auto pCloudManagerState = mp_CloudManagers.f_FindEqual(CloudManagerWeak);
					if (!pCloudManagerState)
						co_return {};

					pCloudManagerState->m_ExpecteOsVersions.f_ApplyChanges(_Versions);

					CCloudManager::CExpectedVersions AllExpectedVersions;
					for (auto &State : mp_CloudManagers)
					{
						for (auto &Version : State.m_ExpecteOsVersions.m_Versions)
						{
							auto &CurrentVersion = State.m_ExpecteOsVersions.m_Versions.fs_GetKey(Version);
							auto *pOldVersion = AllExpectedVersions.m_Versions.f_FindEqual(CurrentVersion);
							if (pOldVersion)
							{
								if (*pOldVersion != Version)
								{
									DMibLogWithCategory
										(
											Malterlib/Cloud/AppManager
											, Warning
											, "Cloud managers have conflicting expected OS version for '{} {}'. '{}' != '{}'"
											, mp_OsName
											, CurrentVersion
											, *pOldVersion
											, Version
										)
									;
								}
							}
							else
								AllExpectedVersions.m_Versions[CurrentVersion] = Version;
						}
					}

					if (mp_HostMonitor)
					{
						co_await mp_HostMonitor(&CHostMonitor::f_SetExpectedOsVersions, fg_Move(AllExpectedVersions)).f_Wrap()
							> fg_LogWarning("Malterlib/Cloud/AppManager", "Failed to update expected OS versions in host monitor")
						;
					}

					co_return {};
				}
			;

			auto ExpectedOsVersionSubscriptionResult = co_await _CloudManager.f_CallActor(&CCloudManager::f_SubscribeExpectedOsVersions)(fg_Move(SubscribeParams)).f_Wrap();

			if (ExpectedOsVersionSubscriptionResult)
			{
				if (ExpectedOsVersionSubscriptionResult)
					ExpectedOsVersionSubscription = fg_Move(*ExpectedOsVersionSubscriptionResult);
				else
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to subscribe to expecte OS versions: {}", ExpectedOsVersionSubscriptionResult.f_GetExceptionStr());
			}
		}

		pNewManager->m_ExpectedOsVersionSubscription = fg_Move(ExpectedOsVersionSubscription);

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_CloudManagerRemoved(TCWeakDistributedActor<CActor> _CloudManager)
	{
		auto pCloudManagerState = mp_CloudManagers.f_FindEqual(_CloudManager);
		if (!pCloudManagerState)
			co_return {};

		TCFutureVector<void> DestroyResults;

		if (pCloudManagerState->m_AppManagerCloudManagerInterface)
			fg_Move(pCloudManagerState->m_AppManagerCloudManagerInterface).f_Destroy() > DestroyResults;

		if (pCloudManagerState->m_SensorReporterSubscription)
			fg_Exchange(pCloudManagerState->m_SensorReporterSubscription, nullptr)->f_Destroy() > DestroyResults;

		if (pCloudManagerState->m_LogReporterSubscription)
			fg_Exchange(pCloudManagerState->m_LogReporterSubscription, nullptr)->f_Destroy() > DestroyResults;

		if (pCloudManagerState->m_ExpectedOsVersionSubscription)
			fg_Exchange(pCloudManagerState->m_ExpectedOsVersionSubscription, nullptr)->f_Destroy() > DestroyResults;

		mp_CloudManagers.f_Remove(_CloudManager);

		co_await fg_AllDone(DestroyResults);

		co_return {};
	}
}
