// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_SubscribeCoordinationInterface()
	{
		mp_RemoteAppManagers = co_await mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_SubscribeTrustedActors<CAppManagerCoordinationInterface>
				, CAppManagerCoordinationInterface::mc_pDefaultNamespace
				, fg_ThisActor(this)
			)
		;

		mp_RemoteAppManagers.f_OnActor
			(
				[this](TCDistributedActor<CAppManagerCoordinationInterface> const &_NewActor, CTrustedActorInfo const &_ActorInfo)
				{
					auto &RemoteActor = mp_RemoteAppManagerState[_ActorInfo.m_HostInfo.m_HostID];
					if (RemoteActor.m_ByActorLink.f_IsInTree())
						mp_RemoteAppManagerStateByActor.f_Remove(RemoteActor);
					RemoteActor.m_Actor = _NewActor;
					RemoteActor.m_HostInfo = _ActorInfo;
					mp_RemoteAppManagerStateByActor.f_Insert(RemoteActor);
					RemoteActor.m_bInitialStateReceived = false;
					fp_NewRemoteAppManager(RemoteActor) > fg_LogError("Malterlib/Cloud/AppManager", "Failed to process new remote app manager");
				}
			)
		;

		mp_RemoteAppManagers.f_OnRemoveActor
			(
				[this](TCWeakDistributedActor<CActor> const &_RemovedActor)
				{
					auto pActor = mp_RemoteAppManagerStateByActor.f_FindEqual(_RemovedActor);
					if (!pActor)
						return;
					mp_RemoteAppManagerStateByActor.f_Remove(pActor);
					mp_RemoteAppManagerState.f_Remove(pActor);
				}
			)
		;

		co_return {};
	}

	void CAppManagerActor::fp_OnAppUpdateInfoChange()
	{
		TCVector<TCSharedPointerSupportWeak<COnAppUpdateInfoChange>> ToProcess;
		for (auto &pOnChange : mp_OnAppUpdateInfoChange)
			ToProcess.f_Insert(pOnChange);
		
		for (auto &pOnChange : ToProcess)
			pOnChange->m_fOnChanged();
	}
	
	TCFutureAllowReferences<void> CAppManagerActor::fp_NewRemoteAppManager(CRemoteAppManager &_AppManager)
	{	
		CStr HostID = _AppManager.f_GetHostID();
		auto Actor = _AppManager.m_Actor;
		
		auto Subscription = co_await DMibCallActor
			(
				Actor
				, CAppManagerCoordinationInterface::f_SubscribeToAppChanges
				, g_ActorFunctor / [this, HostID, AllowDestroy = g_AllowWrongThreadDestroy]
				(TCVector<CAppManagerCoordinationInterface::CAppChange> const &_Changes, bool _bInitial) -> TCFuture<void>
				{
					auto &RemoteAppManager = mp_RemoteAppManagerState[HostID];
					
					if (_bInitial)
					{
						RemoteAppManager.f_Clear();
						RemoteAppManager.m_bInitialStateReceived = true;
					}
					
					for (auto &Change : _Changes)
					{
						switch (Change.f_GetTypeID())
						{
						case CAppManagerCoordinationInterface::EAppChange_Update:
							{
								auto &ChangeData = Change.f_Get<CAppManagerCoordinationInterface::EAppChange_Update>();
								RemoteAppManager.m_AppInfos[ChangeData.m_Application].m_AppInfo = ChangeData;

								CRemoteApplicationKey RemoteKey{ChangeData};
								
								if (mp_KnownRemoteApplications[RemoteKey](HostID).f_WasCreated())
									fp_NewRemoteKnownApplication(RemoteKey, HostID);
								break;
							}
						case CAppManagerCoordinationInterface::EAppChange_Remove:
							{
								auto &ChangeData = Change.f_Get<CAppManagerCoordinationInterface::EAppChange_Remove>();
								RemoteAppManager.m_AppInfos.f_Remove(ChangeData.m_Application); 
								break;
							}
						case CAppManagerCoordinationInterface::EAppChange_AddKnownHosts:
							{
								auto &ChangeData = Change.f_Get<CAppManagerCoordinationInterface::EAppChange_AddKnownHosts>();

								CRemoteApplicationKey RemoteKey;
								RemoteKey.m_Group = ChangeData.m_Group;
								RemoteKey.m_VersionManagerApplication = ChangeData.m_VersionManagerApplication;
								
								RemoteAppManager.m_KnownApplications[RemoteKey] += ChangeData.m_KnownHosts;
								break;
							}
						}
					}
					
					fp_OnAppUpdateInfoChange();
					return fg_Explicit();
				}
			)
		;

		auto &RemoteAppManager = mp_RemoteAppManagerState[HostID];
		RemoteAppManager.m_OnChangeSubscription = fg_Move(Subscription);

		co_return {};
	}
	
	auto CAppManagerActor::CAppManagerCoordinationInterfaceImplementation::f_SubscribeToAppChanges
		(
			TCActorFunctorWithID<TCFuture<void> (TCVector<CAppChange> const &_Changes, bool _bInitial)> &&_fOnChange
		)
		-> TCFuture<TCActorSubscriptionWithID<>>
	{
		auto pThis = m_pThis;
		CStr SubscriptionID = fg_RandomID();
		CStr CallingHostID = fg_GetCallingHostID();

		auto &RemoteAppManager = pThis->mp_RemoteAppManagerState[CallingHostID];
		RemoteAppManager.m_fOnChange = fg_Move(_fOnChange);
		mint SubscriptionSequence = ++RemoteAppManager.m_iOnChangeSubscriptionSequence;
		
		pThis->fp_SendInitialInfoToRemoteAppManager(RemoteAppManager);
		
		co_return g_ActorSubscription / [pThis, CallingHostID, SubscriptionSequence]() -> TCFuture<void>
			{
				auto &RemoteAppManager = pThis->mp_RemoteAppManagerState[CallingHostID];
				if (RemoteAppManager.m_iOnChangeSubscriptionSequence != SubscriptionSequence)
					return fg_Explicit();

				TCFuture<void> DestroyFuture = RemoteAppManager.m_fOnChange.f_Destroy();

				RemoteAppManager.m_fOnChange.f_Clear();

				return DestroyFuture;
			}
		;
	}

	TCFuture<void> CAppManagerActor::CAppManagerCoordinationInterfaceImplementation::f_RemoveKnownHost(CStr const &_Group, CStr const &_Application, CStr const &_HostID)
	{
		auto pThis = m_pThis;

		auto fRemoveRemote = [&](TCMap<CRemoteApplicationKey, TCSet<CStr>> &o_KnownApplications, TCFunction<void (CRemoteApplicationKey const &_Key)> &&_fOnRemove)
			{
				for (auto iKnownHosts = o_KnownApplications.f_GetIterator(); iKnownHosts;)
				{
					auto &RemoteKey = iKnownHosts.f_GetKey();
					
					if (!_Group.f_IsEmpty() && RemoteKey.m_Group != _Group)
						continue;
					if (!_Application.f_IsEmpty() && RemoteKey.m_VersionManagerApplication != _Application)
						continue;
					if (iKnownHosts->f_Remove(_HostID))
					{
						if (_fOnRemove)
							_fOnRemove(RemoteKey);
						if (iKnownHosts->f_IsEmpty())
						{
							iKnownHosts.f_Remove();
							continue;
						}
					}
					++iKnownHosts;
				}
			}
		;
		bool bChanged = false;
		auto &KnowApplicationsState = pThis->mp_State.m_StateDatabase.m_Data["KnownRemoteApplications"];
		fRemoveRemote
			(
				pThis->mp_KnownRemoteApplications
				, [&](CRemoteApplicationKey const &_RemoteKey)
				{
					CEJSON *pDBGroup = KnowApplicationsState.f_GetMember(_RemoteKey.m_Group, EJSONType_Object);
					CEJSON *pDBApplication = nullptr;
					if (pDBGroup)
						pDBApplication = pDBGroup->f_GetMember(_RemoteKey.m_VersionManagerApplication, EJSONType_Object);
					if (pDBApplication && pDBApplication->f_RemoveMember(_HostID))
					{
						bChanged = true;
						if (pDBApplication->f_Object().f_IsEmpty())
						{
							pDBGroup->f_RemoveMember(_RemoteKey.m_VersionManagerApplication);
							if (pDBGroup->f_Object().f_IsEmpty())
								KnowApplicationsState.f_RemoveMember(_RemoteKey.m_Group);
						}
					}
				}
			)
		;
		
		for (auto &RemoteAppManager : pThis->mp_RemoteAppManagerState)
			fRemoveRemote(RemoteAppManager.m_KnownApplications, nullptr);
			
		if (!bChanged)
			co_return {};
		
		pThis->mp_State.m_StateDatabase.m_Data["KnownRemoteApplications"].f_RemoveMember(_HostID);
		co_await pThis->fp_SaveStateDatabase();

		co_return {};
	}

	void CAppManagerActor::CRemoteAppManager::f_Clear()
	{
		m_KnownApplications.f_Clear();
		m_AppInfos.f_Clear();
	}
	
	EDistributedAppUpdateType CAppManagerActor::CApplication::f_GetUpdateType() const
	{
		if (m_Settings.m_bSelfUpdateSource)
			return EDistributedAppUpdateType_OneAtATime;
		
		return m_RegisterInfo.m_UpdateType;
	}

	CAppManagerCoordinationInterface::CAppInfo CAppManagerActor::CApplication::f_GetRemoteAppInfo() const
	{
		CAppManagerCoordinationInterface::CAppInfo AppInfo;
		AppInfo.m_Group = m_Settings.m_UpdateGroup;
		AppInfo.m_VersionManagerApplication = m_Settings.m_VersionManagerApplication;
		AppInfo.m_VersionID = m_LastInstalledVersionFinished;
		AppInfo.m_VersionTime = m_LastInstalledVersionInfoFinished.m_Time; 
		AppInfo.m_FailedVersionID = m_LastFailedInstalledVersion;
		AppInfo.m_FailedVersionTime = m_LastFailedInstalledVersionTime;
		AppInfo.m_FailedVersionRetrySequence = m_LastFailedInstalledVersionRetrySequence;
		AppInfo.m_UpdateStage = m_UpdateStage;
		AppInfo.m_WantUpdateStage = m_WantUpdateStage;
		AppInfo.m_UpdateType = f_GetUpdateType();
		AppInfo.m_UpdateStartSequence = m_UpdateStartSequence;
		return AppInfo;
	}
	
	void CAppManagerActor::fp_NewRemoteKnownApplication(CRemoteApplicationKey const &_RemoteKey, CStr const &_HostID)
	{
		CAppManagerCoordinationInterface::CAppChange_AddKnownHosts AppChange;
		AppChange.m_Group = _RemoteKey.m_Group;
		AppChange.m_VersionManagerApplication = _RemoteKey.m_VersionManagerApplication;
		AppChange.m_KnownHosts[_HostID];
		
		TCVector<CAppManagerCoordinationInterface::CAppChange> Changes;
		Changes.f_Insert(fg_Move(AppChange));
		
		for (auto &RemoteAppManager : mp_RemoteAppManagerState)
		{
			if (!RemoteAppManager.m_fOnChange)
				continue;
			RemoteAppManager.m_fOnChange(Changes, false) > fg_DiscardResult();
		}
		
		mp_State.m_StateDatabase.m_Data["KnownRemoteApplications"][_RemoteKey.m_Group][_RemoteKey.m_VersionManagerApplication][_HostID] = true;
		fp_SaveStateDatabase() > fg_DiscardResult();
	}
	
	void CAppManagerActor::fp_BroadcastRemoteAppChange(CAppManagerCoordinationInterface::CAppChange &&_Change)
	{
		TCVector<CAppManagerCoordinationInterface::CAppChange> Changes;
		Changes.f_Insert(fg_Move(_Change));
		
		for (auto &RemoteAppManager : mp_RemoteAppManagerState)
		{
			if (!RemoteAppManager.m_fOnChange)
				continue;
			RemoteAppManager.m_fOnChange(Changes, false) > fg_DiscardResult();
		}
	}
	
	void CAppManagerActor::fp_SendAppToRemoteAppManagers(TCSharedPointer<CApplication> const &_pApplication)
	{
		auto &Application = *_pApplication;
		CAppManagerCoordinationInterface::CAppChange_Update Change;
		Change.m_Application = Application.m_Name;
		Change = Application.f_GetRemoteAppInfo();
		
		fp_BroadcastRemoteAppChange(fg_Move(Change));

		CRemoteApplicationKey RemoteKey{Change};
		
		if (mp_KnownRemoteApplications[RemoteKey](mp_State.m_HostID).f_WasCreated())
			fp_NewRemoteKnownApplication(RemoteKey, mp_State.m_HostID);
	}
	
	void CAppManagerActor::fp_SendRemovedAppToRemoteAppManagers(TCSharedPointer<CApplication> const &_pApplication)
	{
		auto &Application = *_pApplication;
		CAppManagerCoordinationInterface::CAppChange_Remove Change;
		Change.m_Application = Application.m_Name;
		
		fp_BroadcastRemoteAppChange(fg_Move(Change));
	}
	
	void CAppManagerActor::fp_OnAppUpdateInfoChange(TCSharedPointer<CApplication> const &_pApplication)
	{
		fp_SendAppToRemoteAppManagers(_pApplication);
		fp_OnAppUpdateInfoChange(); // Process our own changes		
	}
	
	void CAppManagerActor::fp_SendInitialInfoToRemoteAppManager(CRemoteAppManager &_AppManager)
	{
		DRequire(_AppManager.m_fOnChange);
		TCVector<CAppManagerCoordinationInterface::CAppChange> Changes;
		
		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;
			if (Application.m_Settings.m_VersionManagerApplication.f_IsEmpty())
				continue;
			
			auto &Change = Changes.f_Insert().f_Set<CAppManagerCoordinationInterface::EAppChange_Update>();
			Change.m_Application = Application.m_Name;
			Change = Application.f_GetRemoteAppInfo();
		}
		
		for (auto &KnownHosts : mp_KnownRemoteApplications)
		{
			auto &RemoteKey = mp_KnownRemoteApplications.fs_GetKey(KnownHosts);
			
			auto &Change = Changes.f_Insert().f_Set<CAppManagerCoordinationInterface::EAppChange_AddKnownHosts>();
			Change.m_Group = RemoteKey.m_Group;
			Change.m_VersionManagerApplication = RemoteKey.m_VersionManagerApplication;
			Change.m_KnownHosts = KnownHosts;
		}
		
		_AppManager.m_fOnChange(fg_Move(Changes), true) > fg_DiscardResult();
	}
}
