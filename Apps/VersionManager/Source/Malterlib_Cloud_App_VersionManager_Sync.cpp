// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/String/Algorithm>
#include <Mib/File/File>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	using namespace NMib::NStr;

	bool CVersionManagerDaemonActor::CServer::fs_SyncMatchesFilter(CStr const &_Value, TCVector<CStr> const &_Filters)
	{
		if (_Filters.f_IsEmpty())
			return true;
		for (auto &Filter : _Filters)
		{
			if (fg_StrMatchWildcard(_Value.f_GetStr(), Filter.f_GetStr()) == EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
				return true;
		}
		return false;
	}

	bool CVersionManagerDaemonActor::CServer::fs_SyncMatchesTagFilter(TCSet<CStr> const &_Tags, TCVector<CStr> const &_Filters)
	{
		if (_Filters.f_IsEmpty())
			return true;
		for (auto &Tag : _Tags)
		{
			for (auto &Filter : _Filters)
			{
				if (fg_StrMatchWildcard(Tag.f_GetStr(), Filter.f_GetStr()) == EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
					return true;
			}
		}
		return false;
	}

	bool CVersionManagerDaemonActor::CServer::fs_SyncNotificationMatchesConfigFilters(CVersionManager::CNewVersionNotification const &_Notification, CSyncSourceConfig const &_Config)
	{
		if (!fs_SyncMatchesFilter(_Notification.m_Application, _Config.m_ApplicationFilters))
			return false;

		if (!fs_SyncMatchesFilter(_Notification.m_VersionIDAndPlatform.m_Platform, _Config.m_PlatformFilters))
			return false;

		CStr VersionIDStr = CStr::fs_ToStr(_Notification.m_VersionIDAndPlatform.m_VersionID);
		if (!fs_SyncMatchesFilter(VersionIDStr, _Config.m_VersionFilters))
			return false;

		if (!fs_SyncMatchesTagFilter(_Notification.m_VersionInfo.m_Tags, _Config.m_TagFilters))
			return false;

		return true;
	}

	TCSet<CStr> CVersionManagerDaemonActor::CServer::fs_SyncGetConfigOwnedTags(TCMap<CStr, CStr> const &_CopyTagMappings)
	{
		TCSet<CStr> OwnedTags;
		for (auto &Mapping : _CopyTagMappings)
			OwnedTags[Mapping];
		return OwnedTags;
	}

	TCSet<CStr> CVersionManagerDaemonActor::CServer::fs_SyncGetPresentTags(TCSet<CStr> const &_SourceTags, TCMap<CStr, CStr> const &_CopyTagMappings)
	{
		TCSet<CStr> PresentTags;
		for (auto &Tag : _SourceTags)
		{
			if (auto *pMapping = _CopyTagMappings.f_FindEqual(Tag))
				PresentTags[*pMapping];
		}
		return PresentTags;
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncRegisterSensors()
	{
		for (auto &SyncSource : mp_SyncSources)
		{
			if (SyncSource.m_bSensorsRegistered)
				continue;

			CDistributedAppSensorReporter::CSensorInfo SensorInfo;
			SensorInfo.m_Identifier = "org.malterlib.cloud.versionmanager.sync.status";
			SensorInfo.m_IdentifierScope = SyncSource.f_GetName();
			SensorInfo.m_Name = "Version Manager Sync Status ({})"_f << SyncSource.f_GetName();
			SensorInfo.m_Type = CDistributedAppSensorReporter::ESensorDataType_Status;

			SyncSource.m_SensorReporter_Status = co_await mp_AppState.f_OpenSensorReporter(fg_Move(SensorInfo));
			SyncSource.m_bSensorsRegistered = true;
		}

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncUpdateSensorStatus(CStr _ConfigName)
	{
		auto *pSyncSource = mp_SyncSources.f_FindEqual(_ConfigName);
		if (!pSyncSource)
			co_return {};

		if (!pSyncSource->m_SensorReporter_Status.m_fReportReadings)
			co_return {};

		CDistributedAppSensorReporter::CStatus Status;

		if (pSyncSource->m_HostErrors.f_IsEmpty())
		{
			Status.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Ok;
			Status.m_Description = "All {} hosts connected"_f << pSyncSource->m_Config.m_SyncHosts.f_GetLen();
		}
		else
		{
			Status.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Error;

			CStr ErrorDescription = "Errors: ";
			bool bFirst = true;
			for (auto &HostError : pSyncSource->m_HostErrors)
			{
				if (!bFirst)
					ErrorDescription += ", ";
				bFirst = false;
				ErrorDescription += TCMap<CStr, CStr>::fs_GetKey(HostError);
				ErrorDescription += ": ";
				ErrorDescription += HostError;
			}
			Status.m_Description = ErrorDescription;
		}

		TCVector<CDistributedAppSensorReporter::CSensorReading> SensorReadings;
		SensorReadings.f_Insert().m_Data = Status;

		co_await pSyncSource->m_SensorReporter_Status.m_fReportReadings(fg_Move(SensorReadings));

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncSetHostError(CStr _ConfigName, CStr _HostID, CStr _ErrorMessage)
	{
		auto *pSyncSource = mp_SyncSources.f_FindEqual(_ConfigName);
		if (!pSyncSource)
			co_return {};

		pSyncSource->m_HostErrors[_HostID] = _ErrorMessage;

		co_await fp_SyncUpdateSensorStatus(_ConfigName);

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncClearHostError(CStr _ConfigName, CStr _HostID)
	{
		auto *pSyncSource = mp_SyncSources.f_FindEqual(_ConfigName);
		if (!pSyncSource)
			co_return {};

		if (!pSyncSource->m_HostErrors.f_FindEqual(_HostID))
			co_return {};

		pSyncSource->m_HostErrors.f_Remove(_HostID);

		co_await fp_SyncUpdateSensorStatus(_ConfigName);

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncInit()
	{
		auto *pSyncSourcesJson = mp_AppState.m_ConfigDatabase.m_Data.f_GetMember("SyncSources", EJsonType_Object);
		if (!pSyncSourcesJson)
		{
			DMibLogWithCategory(Malterlib/Cloud/VersionManager/Sync, Info, "No SyncSources configured");
			co_return {};
		}

		for (auto &SourceEntry : pSyncSourcesJson->f_Object())
		{
			CStr ConfigName = SourceEntry.f_Name();
			auto &SourceJson = SourceEntry.f_Value();

			CSyncSourceConfig Config;
			Config.m_Name = ConfigName;

			if (auto *pEnabled = SourceJson.f_GetMember("Enabled", EJsonType_Boolean))
				Config.m_bEnabled = pEnabled->f_Boolean();

			if (!Config.m_bEnabled)
			{
				DMibLogWithCategory(Malterlib/Cloud/VersionManager/Sync, Info, "Sync source '{}' is disabled", ConfigName);
				continue;
			}

			if (auto *pPretend = SourceJson.f_GetMember("Pretend", EJsonType_Boolean))
				Config.m_bPretend = pPretend->f_Boolean();

			if (auto *pSyncHosts = SourceJson.f_GetMember("SyncHosts", EJsonType_Array))
			{
				for (auto &Host : pSyncHosts->f_Array())
				{
					if (Host.f_IsString())
						Config.m_SyncHosts[Host.f_String()];
				}
			}

			if (Config.m_SyncHosts.f_IsEmpty())
			{
				DMibLogWithCategory(Malterlib/Cloud/VersionManager/Sync, Error, "Sync source '{}' has no SyncHosts configured - skipping", ConfigName);
				continue;
			}

			if (auto *pApplications = SourceJson.f_GetMember("Applications", EJsonType_Array))
			{
				for (auto &App : pApplications->f_Array())
				{
					if (App.f_IsString())
						Config.m_ApplicationFilters.f_InsertLast(App.f_String());
				}
			}

			if (auto *pPlatforms = SourceJson.f_GetMember("Platforms", EJsonType_Array))
			{
				for (auto &Platform : pPlatforms->f_Array())
				{
					if (Platform.f_IsString())
						Config.m_PlatformFilters.f_InsertLast(Platform.f_String());
				}
			}

			if (auto *pVersions = SourceJson.f_GetMember("Versions", EJsonType_Array))
			{
				for (auto &Version : pVersions->f_Array())
				{
					if (Version.f_IsString())
						Config.m_VersionFilters.f_InsertLast(Version.f_String());
				}
			}

			if (auto *pTags = SourceJson.f_GetMember("Tags", EJsonType_Array))
			{
				for (auto &Tag : pTags->f_Array())
				{
					if (Tag.f_IsString())
						Config.m_TagFilters.f_InsertLast(Tag.f_String());
				}
			}

			if (auto *pCopyTags = SourceJson.f_GetMember("CopyTags", EJsonType_Array))
			{
				for (auto &CopyTag : pCopyTags->f_Array())
				{
					if (CopyTag.f_IsString())
					{
						CStr CopyTagStr = CopyTag.f_String();
						CStr SourceTag, DestTag;
						aint nVarsParsed = 0;
						aint nCharsParsed = (CStr::CParse("{}={}") >> SourceTag >> DestTag).f_Parse(CopyTagStr, nVarsParsed);
						if (nVarsParsed == 2 && nCharsParsed == CopyTagStr.f_GetLen())
							Config.m_CopyTagMappings[SourceTag] = DestTag;
						else
							Config.m_CopyTagMappings[CopyTagStr] = CopyTagStr;
					}
				}
			}

			if (auto *pSyncRetrySequence = SourceJson.f_GetMember("SyncRetrySequence", EJsonType_Boolean))
				Config.m_bSyncRetrySequence = pSyncRetrySequence->f_Boolean();

			if (auto *pStartSyncDate = SourceJson.f_GetMember("StartSyncDate", EJsonType_String); pStartSyncDate && !pStartSyncDate->f_String().f_IsEmpty())
			{
				auto StartSyncDateStr = pStartSyncDate->f_String();

				auto StartSyncDate = fg_TryParseDateTimeStr(StartSyncDateStr);
				if (!StartSyncDate)
				{
					co_return DMibErrorInstance
						(
							"Invalid StartSyncDate '{}' in sync source '{}': {}"_f
							<< StartSyncDateStr
							<< ConfigName
							<< StartSyncDate
						)
					;
				}

				Config.m_StartSyncDate = *StartSyncDate;
			}

			if (auto *pMinSyncVersions = SourceJson.f_GetMember("MinSyncVersions", EJsonType_Integer))
				Config.m_nMinSyncVersions = (uint32)pMinSyncVersions->f_Integer();

			if (auto *pTransferQueueSize = SourceJson.f_GetMember("TransferQueueSize", EJsonType_Integer))
				Config.m_QueueSize = (uint64)pTransferQueueSize->f_Integer();

			auto &SyncSource = mp_SyncSources[ConfigName];
			SyncSource.m_Config = fg_Move(Config);

			DMibLogWithCategory
				(
					Malterlib/Cloud/VersionManager/Sync
					, Info
					, "Loaded sync source '{}': {} hosts, apps: {}, platforms: {}, pretend: {}"
					, ConfigName
					, SyncSource.m_Config.m_SyncHosts.f_GetLen()
					, SyncSource.m_Config.m_ApplicationFilters.f_GetLen()
					, SyncSource.m_Config.m_PlatformFilters.f_GetLen()
					, SyncSource.m_Config.m_bPretend
				)
			;
		}

		if (mp_SyncSources.f_IsEmpty())
		{
			DMibLogWithCategory(Malterlib/Cloud/VersionManager/Sync, Info, "No enabled sync sources configured");
			co_return {};
		}

		co_await fp_SyncRegisterSensors();

		mp_SyncVersionManagerSubscription = co_await mp_AppState.m_TrustManager->f_SubscribeTrustedActors<CVersionManager>();

		co_await mp_SyncVersionManagerSubscription.f_OnActor
			(
				g_ActorFunctor / [this](TCDistributedActor<CVersionManager> _Manager, CTrustedActorInfo _Info) -> TCFuture<void>
				{
					co_await fp_SyncOnVersionManagerAdded(fg_Move(_Manager), fg_Move(_Info));
					co_return {};
				}
				, g_ActorFunctor / [this](TCWeakDistributedActor<CActor> _Manager, CTrustedActorInfo _Info) -> TCFuture<void>
				{
					co_await fp_SyncOnVersionManagerRemoved(fg_Move(_Manager), fg_Move(_Info));
					co_return {};
				}
			)
		;

		TCFutureVector<void> ErrorResults;
		for (auto &SyncSource : mp_SyncSources)
		{
			for (auto &HostID : SyncSource.m_Config.m_SyncHosts)
			{
				if (!mp_SyncHostSubscriptions.f_FindEqual(HostID))
					fp_SyncSetHostError(SyncSource.f_GetName(), HostID, "Host not connected") > ErrorResults;
			}
		}

		co_await fg_AllDone(ErrorResults).f_Wrap() > fg_LogError("VersionManager/Sync", "Failed to update host error status");

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncDestroy()
	{
		CLogError LogError("Malterlib/Cloud/VersionManager/Sync");

		TCFutureVector<void> DestroyResults;

		for (auto &HostSubscription : mp_SyncHostSubscriptions)
			fg_Move(HostSubscription.m_Subscription)->f_Destroy() > DestroyResults;
		mp_SyncHostSubscriptions.f_Clear();

		for (auto &SyncSource : mp_SyncSources)
			fg_Move(SyncSource.m_SensorReporter_Status.m_fReportReadings).f_Destroy() > DestroyResults;

		mp_SyncSources.f_Clear();

		for (auto &VersionState : mp_SyncVersionStates)
			fg_Move(VersionState.m_Sequencer).f_Destroy() > DestroyResults;

		co_await fg_AllDone(DestroyResults).f_Wrap() > LogError.f_Warning("Failed to destroy sync subscriptions");

		co_await fg_Move(mp_SyncVersionManagerSubscription).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy version manager subscription");

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncOnVersionManagerAdded(TCDistributedActor<CVersionManager> _Manager, CTrustedActorInfo _Info)
	{
		CStr HostID = _Info.m_UniqueHostID;

		bool bHostNeeded = false;
		for (auto &SyncSource : mp_SyncSources)
		{
			if (SyncSource.m_Config.m_SyncHosts.f_FindEqual(HostID))
			{
				bHostNeeded = true;
				break;
			}
		}

		if (!bHostNeeded)
			co_return {};

		auto &HostSubscription = mp_SyncHostSubscriptions[HostID];
		auto WeakManager = _Manager.f_Weak();

		CActorSubscription OldSubscription = fg_Move(HostSubscription.m_Subscription);

		HostSubscription.m_Manager = WeakManager;

		if (OldSubscription)
		{
			DMibLogWithCategory(Malterlib/Cloud/VersionManager/Sync, Info,
				"Re-subscribing to version manager from host '{}' (reconnected)", HostID);
			fg_Move(OldSubscription)->f_Destroy() > fg_LogWarning("VersionManager/Sync", "Failed to destroy old subscription");
		}
		else
		{
			DMibLogWithCategory(Malterlib/Cloud/VersionManager/Sync, Info,
				"Subscribing to version manager from host '{}'", HostID);
		}

		CVersionManager::CSubscribeToUpdates SubscriptionParams;
		SubscriptionParams.m_Application = CStr(); // All applications (filter locally for security)
		SubscriptionParams.m_nInitial = TCLimitsInt<uint32>::mc_Max;

		SubscriptionParams.m_fOnNewVersions = g_ActorFunctor / [this, HostID, WeakManager, AllowDestroy = g_AllowWrongThreadDestroy]
			(CVersionManager::CNewVersionNotifications _Notifications)
			-> NConcurrency::TCFuture<CVersionManager::CNewVersionNotifications::CResult>
			{
				co_await fp_SyncOnNewVersions(HostID, WeakManager, fg_Move(_Notifications));
				co_return {};
			}
		;

		auto SubscriptionResult = co_await _Manager.f_CallActor(&CVersionManager::f_SubscribeToUpdates)(fg_Move(SubscriptionParams)).f_Wrap();
		if (!SubscriptionResult)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/VersionManager/Sync
					, Error
					, "Failed to subscribe to version manager from host '{}': {}"
					, HostID
					, SubscriptionResult.f_GetExceptionStr()
				)
			;
			co_return {};
		}

		auto *pCurrentHostSubscription = mp_SyncHostSubscriptions.f_FindEqual(HostID);
		if (!pCurrentHostSubscription || pCurrentHostSubscription->m_Manager != _Manager)
		{
			fg_Move(SubscriptionResult->m_Subscription)->f_Destroy() > fg_LogWarning("VersionManager/Sync", "Failed to destroy superseded subscription");
			co_return {};
		}

		pCurrentHostSubscription->m_Subscription = fg_Move(SubscriptionResult->m_Subscription);

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncOnVersionManagerRemoved(TCWeakDistributedActor<CActor> _Manager, CTrustedActorInfo _Info)
	{
		CStr HostID = _Info.m_UniqueHostID;

		auto *pHostSubscription = mp_SyncHostSubscriptions.f_FindEqual(HostID);
		if (pHostSubscription && pHostSubscription->m_Manager == _Manager)
		{
			DMibLogWithCategory(Malterlib/Cloud/VersionManager/Sync, Info,
				"Version manager from host '{}' disconnected", HostID);

			fg_Move(pHostSubscription->m_Subscription)->f_Destroy() > fg_LogWarning("VersionManager/Sync", "Failed to destroy subscription on removal");
			mp_SyncHostSubscriptions.f_Remove(HostID);

			TCFutureVector<void> ErrorResults;
			for (auto &SyncSource : mp_SyncSources)
			{
				if (SyncSource.m_Config.m_SyncHosts.f_FindEqual(HostID))
					fp_SyncSetHostError(SyncSource.f_GetName(), HostID, "Host not connected") > ErrorResults;
			}

			co_await fg_AllDone(ErrorResults).f_Wrap() > fg_LogError("VersionManager/Sync", "Failed to update host error status");
		}

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncOnNewVersions
		(
			CStr _HostID
			, TCWeakDistributedActor<CVersionManager> _Manager
			, CVersionManager::CNewVersionNotifications _Notifications
		)
	{
		auto StrongManager = _Manager.f_Lock();
		if (!StrongManager)
			co_return {};

		TCMap<CStr, TCSet<CSyncVersionKey>> DateBypassByConfig;

		if (_Notifications.m_bFullResend)
		{
			for (auto &SyncSource : mp_SyncSources)
			{
				auto const &Config = SyncSource.m_Config;

				if (Config.m_nMinSyncVersions == 0)
					continue;

				if (!Config.m_SyncHosts.f_FindEqual(_HostID))
					continue;

				TCMap<CStr, TCVector<CVersionManager::CNewVersionNotification const *>> VersionsByApp;

				for (auto const &Notification : _Notifications.m_NewVersions)
				{
					if (!fs_SyncNotificationMatchesConfigFilters(Notification, Config))
						continue;

					VersionsByApp[Notification.m_Application].f_InsertLast(&Notification);
				}

				auto &ConfigBypassSet = DateBypassByConfig[SyncSource.f_GetName()];

				for (auto &AppVersions : VersionsByApp)
				{
					auto &Versions = AppVersions;

					Versions.f_Sort
						(
							[](auto *_pLeft, auto *_pRight)
							{
								auto const &Left = _pLeft->m_VersionInfo.m_Time;
								auto const &Right = _pRight->m_VersionInfo.m_Time;

								if (auto Cmp = !!Right.f_IsValid() <=> !!Left.f_IsValid(); Cmp != 0)
									return Cmp;
								return Right <=> Left;
							}
						)
					;

					umint nVersions = Config.m_nMinSyncVersions;
					for (auto pNotification : Versions)
					{
						ConfigBypassSet[CSyncVersionKey{pNotification->m_Application, pNotification->m_VersionIDAndPlatform}];
						if (--nVersions == 0)
							break;
					}
				}
			}
		}

		bool bHadError = false;
		for (auto &Notification : _Notifications.m_NewVersions)
		{
			auto MatchingConfigs = fp_SyncGetMatchingConfigs(_HostID, Notification, DateBypassByConfig);
			if (MatchingConfigs.f_IsEmpty())
				continue;

			CStr Application = Notification.m_Application;
			CVersionManager::CVersionIDAndPlatform VersionIDAndPlatform = Notification.m_VersionIDAndPlatform;

			auto Result = co_await fp_SyncProcessVersion(_HostID, StrongManager, fg_Move(Notification), MatchingConfigs, _Notifications.m_OriginID).f_Wrap();
			if (!Result)
			{
				DMibLogWithCategory
					(
						Malterlib/Cloud/VersionManager/Sync
						, Error
						, "Sync failed for {} {} ({}) from host '{}': {}"
						, Application
						, VersionIDAndPlatform.m_VersionID
						, VersionIDAndPlatform.m_Platform
						, _HostID
						, Result.f_GetExceptionStr()
					)
				;

				bHadError = true;

				TCFutureVector<void> ErrorResults;
				for (auto &Matching : MatchingConfigs)
					fp_SyncSetHostError(Matching.m_Name, _HostID, Result.f_GetExceptionStr()) > ErrorResults;

				co_await fg_AllDone(ErrorResults).f_Wrap() > fg_LogError("VersionManager/Sync", "Failed to update host error status");
			}
		}

		if (_Notifications.m_bFullResend && !bHadError)
		{
			TCFutureVector<void> ErrorResults;
			for (auto &SyncSource : mp_SyncSources)
			{
				if (SyncSource.m_Config.m_SyncHosts.f_FindEqual(_HostID))
					fp_SyncClearHostError(SyncSource.f_GetName(), _HostID) > ErrorResults;
			}

			co_await fg_AllDone(ErrorResults).f_Wrap() > fg_LogError("VersionManager/Sync", "Failed to update host error status");
		}

		co_return {};
	}

	TCVector<CVersionManagerDaemonActor::CServer::CMatchingConfig> CVersionManagerDaemonActor::CServer::fp_SyncGetMatchingConfigs
		(
			CStr const &_HostID
			, CVersionManager::CNewVersionNotification const &_Notification
			, TCMap<CStr, TCSet<CSyncVersionKey>> const &_DateBypassByConfig
		)
	{
		CSyncVersionKey VersionKey{_Notification.m_Application, _Notification.m_VersionIDAndPlatform};

		TCVector<CMatchingConfig> MatchingConfigs;

		for (auto &SyncSource : mp_SyncSources)
		{
			auto const &Config = SyncSource.m_Config;

			if (!Config.m_SyncHosts.f_FindEqual(_HostID))
				continue;

			if (!fs_SyncNotificationMatchesConfigFilters(_Notification, Config))
				continue;

			if (Config.m_StartSyncDate.f_IsValid() && _Notification.m_VersionInfo.m_Time.f_IsValid())
			{
				if (_Notification.m_VersionInfo.m_Time < Config.m_StartSyncDate)
				{
					auto *pConfigBypass = _DateBypassByConfig.f_FindEqual(SyncSource.f_GetName());
					if (!pConfigBypass || !pConfigBypass->f_FindEqual(VersionKey))
						continue;
				}
			}

			MatchingConfigs.f_InsertLast(CMatchingConfig{SyncSource.f_GetName(), &Config});
		}

		return MatchingConfigs;
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncProcessVersion
		(
			CStr _HostID
			, TCDistributedActor<CVersionManager> _SourceManager
			, CVersionManager::CNewVersionNotification _Notification
			, TCVector<CMatchingConfig> _MatchingConfigs
			, CStr _OriginID
		)
	{
		if (_MatchingConfigs.f_IsEmpty())
			co_return {};

		TCSet<CStr> AllOwnedTags;
		TCSet<CStr> ShouldBePresentTags;
		bool bAnySyncRetrySequence = false;

		for (auto &Matching : _MatchingConfigs)
		{
			if (Matching.m_pConfig->m_bPretend)
				continue;

			AllOwnedTags = AllOwnedTags | fs_SyncGetConfigOwnedTags(Matching.m_pConfig->m_CopyTagMappings);
			ShouldBePresentTags = ShouldBePresentTags | fs_SyncGetPresentTags(_Notification.m_VersionInfo.m_Tags, Matching.m_pConfig->m_CopyTagMappings);

			if (Matching.m_pConfig->m_bSyncRetrySequence)
				bAnySyncRetrySequence = true;
		}

		auto fGetVersion = [&] -> CVersion *
			{
				if (auto *pApp = mp_Applications.f_FindEqual(_Notification.m_Application))
					return pApp->m_Versions.f_FindEqual(_Notification.m_VersionIDAndPlatform);

				return nullptr;
			}
		;

		CSyncVersionKey Key{_Notification.m_Application, _Notification.m_VersionIDAndPlatform};
		auto &VersionState = *mp_SyncVersionStates(Key, fg_Format("SyncVersion {} {}", _Notification.m_Application, _Notification.m_VersionIDAndPlatform.f_EncodeFileName()));

		TCSet<CStr> *pCurrentTags = nullptr;
		uint32 CurrentRetrySequence = 0;
		if (auto *pVersion = fGetVersion())
		{
			pCurrentTags = &pVersion->m_VersionInfo.m_Tags;
			CurrentRetrySequence = pVersion->m_VersionInfo.m_RetrySequence;
		}
		else if (VersionState.m_CurrentOriginID)
		{
			pCurrentTags = &VersionState.m_StoringTags;
			CurrentRetrySequence = VersionState.m_StoringRetrySequence;
		}

		if (pCurrentTags)
		{
			auto FinalTags = (*pCurrentTags - AllOwnedTags) | ShouldBePresentTags;
			bool bTagsAlreadyCorrect = (*pCurrentTags == FinalTags);
			bool bRetrySequenceAlreadyCorrect = !bAnySyncRetrySequence || (_Notification.m_VersionInfo.m_RetrySequence <= CurrentRetrySequence);
			if (bTagsAlreadyCorrect && bRetrySequenceAlreadyCorrect)
				co_return {};
		}


		if (_OriginID && VersionState.m_CurrentOriginID == _OriginID)
		{
			auto FinalTags = (VersionState.m_StoringTags - AllOwnedTags) | ShouldBePresentTags;
			DMibLogWithCategory
				(
					Malterlib/Cloud/VersionManager/Sync
					, Warning
					, "Sync loop detected for {} {} ({}): origin ID '{}' already being processed. Check sync configuration for circular dependencies.\n"
					"    Wanted tags: {vs}\n"
					"    Storing tags: {vs}"
					, _Notification.m_Application
					, _Notification.m_VersionIDAndPlatform.m_VersionID
					, _Notification.m_VersionIDAndPlatform.m_Platform
					, _OriginID
					, FinalTags
					, VersionState.m_StoringTags
				)
			;
			co_return {};
		}

		auto SequenceLock = co_await VersionState.m_Sequencer.f_Sequence();

		DMibFastCheck(VersionState.m_CurrentOriginID.f_IsEmpty());
		VersionState.m_CurrentOriginID = _OriginID;
		auto ClearOriginID = g_OnScopeExit / [&]
			{
				VersionState.m_CurrentOriginID.f_Clear();
				VersionState.m_StoringTags.f_Clear();
				VersionState.m_StoringRetrySequence = 0;
			}
		;

		if (auto *pVersion = fGetVersion())
		{
			auto FinalTags = (pVersion->m_VersionInfo.m_Tags - AllOwnedTags) | ShouldBePresentTags;
			VersionState.m_StoringTags = FinalTags;

			if (bAnySyncRetrySequence)
				VersionState.m_StoringRetrySequence = fg_Max(pVersion->m_VersionInfo.m_RetrySequence, _Notification.m_VersionInfo.m_RetrySequence);

			co_await fp_SyncUpdateLocalTags
				(
					_Notification.m_Application
					, _Notification.m_VersionIDAndPlatform
					, fg_Move(FinalTags)
					, bAnySyncRetrySequence
					, _Notification.m_VersionInfo.m_RetrySequence
					, _OriginID
				)
			;

			co_return {};
		}

		VersionState.m_StoringTags = ShouldBePresentTags;

		CSyncSourceConfig const *pDownloadConfig = nullptr;
		CStr DownloadConfigName;
		for (auto &Matching : _MatchingConfigs)
		{
			if (!Matching.m_pConfig->m_bPretend)
			{
				pDownloadConfig = Matching.m_pConfig;
				DownloadConfigName = Matching.m_Name;
				break;
			}
		}

		for (auto &Matching : _MatchingConfigs)
		{
			if (Matching.m_pConfig->m_bPretend)
			{
				auto PresentTags = fs_SyncGetPresentTags(_Notification.m_VersionInfo.m_Tags, Matching.m_pConfig->m_CopyTagMappings);
				DMibLogWithCategory
					(
						Malterlib/Cloud/VersionManager/Sync
						, Info
						, "PRETEND: Would sync {} {} ({}) from '{}' - {ns } bytes, tags: {} -> {}"
						, _Notification.m_Application
						, _Notification.m_VersionIDAndPlatform.m_VersionID
						, _Notification.m_VersionIDAndPlatform.m_Platform
						, Matching.m_Name
						, _Notification.m_VersionInfo.m_nBytes
						, _Notification.m_VersionInfo.m_Tags.f_GetLen()
						, PresentTags.f_GetLen()
					)
				;
			}
		}

		if (!pDownloadConfig)
			co_return {};

		co_await fp_SyncDownloadAndStoreVersion
			(
				DownloadConfigName
				, *pDownloadConfig
				, _SourceManager
				, _Notification.m_Application
				, _Notification.m_VersionIDAndPlatform
				, _Notification.m_VersionInfo
				, ShouldBePresentTags
				, bAnySyncRetrySequence
				, _OriginID
			)
		;

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncDownloadAndStoreVersion
		(
			CStr _SyncSourceName
			, CSyncSourceConfig _Config
			, TCDistributedActor<CVersionManager> _SourceManager
			, CStr _Application
			, CVersionManager::CVersionIDAndPlatform _VersionID
			, CVersionManager::CVersionInformation _VersionInfo
			, TCSet<CStr> _CombinedTags
			, bool _bSyncRetrySequence
			, CStr _OriginID
		)
	{
		CStr TempDir = mp_AppState.m_RootDirectory / "SyncTemp" / fg_RandomID();

		{
			auto BlockingActorCheckout = fg_BlockingActor();
			co_await
				(
					g_Dispatch(BlockingActorCheckout) / [TempDir]
					{
						NFile::CFile::fs_CreateDirectory(TempDir);
					}
					% "Failed to create temp directory for version sync"
				)
			;
		}

		auto Cleanup = co_await fg_AsyncDestroy
			(
				[TempDir]() -> TCFuture<void>
				{
					auto LocalTempDir = TempDir;
					auto BlockingActorCheckout = fg_BlockingActor();
					co_await
						(
							g_Dispatch(BlockingActorCheckout) / [LocalTempDir]
							{
								try
								{
									NFile::CFile::fs_DeleteDirectoryRecursive(LocalTempDir);
								}
								catch (...)
								{
								}
							}
						)
					;
					co_return {};
				}
			)
		;

		DMibLogWithCategory
			(
				Malterlib/Cloud/VersionManager/Sync
				, Info
				, "Starting sync download of {} {} ({}) from '{}'"
				, _Application
				, _VersionID.m_VersionID
				, _VersionID.m_Platform
				, _SyncSourceName
			)
		;

		CVersionManagerHelper Helper(mp_AppState.m_RootDirectory, _Config.m_QueueSize, 60.0);
		auto DownloadResult = co_await
			(
				Helper.f_Download
				(
					_SourceManager
					, _Application
					, _VersionID
					, TempDir
					, CFileTransferReceive::EReceiveFlag_None
					, _Config.m_QueueSize
				)
				% ("Failed to sync {} {} ({}) from '{}'"_f << _Application << _VersionID.m_VersionID << _VersionID.m_Platform << _SyncSourceName)
			)
		;

		auto DestVersionInfo = _VersionInfo;
		DestVersionInfo.m_Tags = fg_Move(_CombinedTags);
		if (_bSyncRetrySequence)
			DestVersionInfo.m_RetrySequence = fg_Max(DestVersionInfo.m_RetrySequence, _VersionInfo.m_RetrySequence);
		else
			DestVersionInfo.m_RetrySequence = 0;

		CStr ApplicationDir = mp_AppState.m_RootDirectory / "Applications" / _Application;
		CStr VersionPath = ApplicationDir / _VersionID.f_EncodeFileName();

		{
			auto BlockingActorCheckout = fg_BlockingActor();
			co_await
				(
					g_Dispatch(BlockingActorCheckout) / [TempDir, VersionPath]
					{
						NFile::CFile::fs_CreateDirectoryForFile(VersionPath);
						NFile::CFile::fs_RenameFile(TempDir, VersionPath);
					}
					% "Falied to move version into applications folder"
				)
			;
		}

		Cleanup.f_Clear();

		auto SizeInfo = co_await (fp_SaveVersionInfo(VersionPath, DestVersionInfo) % "Failed to save version info");
		DestVersionInfo.m_nFiles = SizeInfo.m_nFiles;
		DestVersionInfo.m_nBytes = SizeInfo.m_nBytes;

		auto ApplicationMapped = mp_Applications(_Application);
		auto &AppEntry = *ApplicationMapped;

		if (ApplicationMapped.f_WasCreated())
		{
			TCSet<CStr> Permissions;
			Permissions[fg_Format("Application/Read/{}", _Application)];
			Permissions[fg_Format("Application/Write/{}", _Application)];
			mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions).f_DiscardResult();
		}

		auto &VersionEntry = AppEntry.m_Versions[_VersionID];
		if (VersionEntry.m_TimeLink.f_IsInTree())
			AppEntry.m_VersionsByTime.f_Remove(VersionEntry);
		VersionEntry.m_VersionInfo = DestVersionInfo;
		AppEntry.m_VersionsByTime.f_Insert(VersionEntry);

		co_await (fp_SaveVersionToDatabase(_Application, _VersionID, DestVersionInfo) % "Failed to save version to database");

		fp_NewTagsKnown(DestVersionInfo.m_Tags);

		co_await fp_NewVersion(_Application, VersionEntry.f_GetIdentifier(), VersionEntry.m_VersionInfo, _OriginID);

		DMibLogWithCategory
			(
				Malterlib/Cloud/VersionManager/Sync
				, Info
				, "Synced {} {} ({}) from '{}' - {ns } at {fe2} MB/s"
				, _Application
				, _VersionID.m_VersionID
				, _VersionID.m_Platform
				, _SyncSourceName
				, DownloadResult.m_nBytes
				, DownloadResult.f_BytesPerSecond() / 1'000'000.0
			)
		;

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SyncUpdateLocalTags
		(
			CStr _Application
			, CVersionManager::CVersionIDAndPlatform _VersionID
			, TCSet<CStr> _TransformedTags
			, bool _bSyncRetrySequence
			, uint32 _RemoteRetrySequence
			, CStr _OriginID
		)
	{
		auto *pApp = mp_Applications.f_FindEqual(_Application);
		if (!pApp)
			co_return {};

		auto *pVersion = pApp->m_Versions.f_FindEqual(_VersionID);
		if (!pVersion)
			co_return {};

		bool bTagsChanged = (pVersion->m_VersionInfo.m_Tags != _TransformedTags);
		bool bRetrySequenceChanged = _bSyncRetrySequence && (_RemoteRetrySequence > pVersion->m_VersionInfo.m_RetrySequence);

		if (!bTagsChanged && !bRetrySequenceChanged)
			co_return {};

		uint32 OldRetrySequence = pVersion->m_VersionInfo.m_RetrySequence;

		pVersion->m_VersionInfo.m_Tags = _TransformedTags;
		if (bRetrySequenceChanged)
			pVersion->m_VersionInfo.m_RetrySequence = _RemoteRetrySequence;

		CStr VersionPath = fg_Format("{}/Applications/{}/{}", mp_AppState.m_RootDirectory, _Application, _VersionID.f_EncodeFileName());
		co_await (fp_SaveVersionInfo(VersionPath, pVersion->m_VersionInfo) % "Failed to save version info");
		co_await (fp_SaveVersionToDatabase(_Application, _VersionID, pVersion->m_VersionInfo) % "Failed to save version to database");

		fp_NewTagsKnown(_TransformedTags);

		co_await fp_NewVersion(_Application, pVersion->f_GetIdentifier(), pVersion->m_VersionInfo, _OriginID);

		DMibLogWithCategory
			(
				Malterlib/Cloud/VersionManager/Sync
				, Info
				, "Updated {} {} ({}) - tags changed: {}, retrySeq: {} -> {}"
				, _Application
				, _VersionID.m_VersionID
				, _VersionID.m_Platform
				, bTagsChanged
				, OldRetrySequence
				, pVersion->m_VersionInfo.m_RetrySequence
			)
		;

		co_return {};
	}
}
