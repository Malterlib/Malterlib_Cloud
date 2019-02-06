// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/Actor/Timer>
#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	CAppDistributionManagerActor::CVersionManagerVersion::CVersionManagerVersion(CVersionManagerState *_pVersionManager)
		: m_pVersionManager(_pVersionManager) 
	{
	}

	CAppDistributionManagerActor::CVersionManagerVersion::~CVersionManagerVersion()
	{
		f_SetApplication(nullptr);
	}
	
	void CAppDistributionManagerActor::CVersionManagerVersion::f_SetApplication(CVersionManagerApplication *_pApplication)
	{
		if (m_pApplication)
		{
			m_pApplication->m_VersionsByTime.f_Remove(*this);
			m_pApplication->m_Versions.f_Remove(*this);
			if (m_pApplication->m_VersionsByTime.f_IsEmpty() & m_pApplication->m_Versions.f_IsEmpty())
				m_pApplication->m_This.mp_VersionManagerApplications.f_Remove(m_pApplication);
			m_pApplication = nullptr;
		}
		if (!_pApplication)
			return;
		_pApplication->m_VersionsByTime.f_Insert(*this);
		_pApplication->m_Versions.f_Insert(*this);
		m_pApplication = _pApplication;
	}
	
	CVersionManager::CVersionIDAndPlatform const &CAppDistributionManagerActor::CVersionManagerVersion::f_GetVersionID() const
	{
		return TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManagerVersion>::fs_GetKey(*this);
	}
	
	bool CAppDistributionManagerActor::CVersionManagerVersion::CCompareApplicationByTime::operator()(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const
	{
		auto *pLeft = &_Left;
		auto *pRight = &_Right;
		if (!_Left.m_VersionInfo.m_Time.f_IsValid() && _Right.m_VersionInfo.m_Time.f_IsValid())
			return true;
		else if (_Left.m_VersionInfo.m_Time.f_IsValid() && !_Right.m_VersionInfo.m_Time.f_IsValid())
			return false;
		return NStorage::fg_TupleReferences(_Left.m_VersionInfo.m_Time, _Left.f_GetVersionID(), pLeft)
			< NStorage::fg_TupleReferences(_Right.m_VersionInfo.m_Time, _Right.f_GetVersionID(), pRight)
		;
	}
	
	bool CAppDistributionManagerActor::CVersionManagerVersion::CCompareApplication::operator()(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const
	{
		auto *pLeft = &_Left;
		auto *pRight = &_Right;
		return NStorage::fg_TupleReferences(_Left.f_GetVersionID(), pLeft)
			< NStorage::fg_TupleReferences(_Right.f_GetVersionID(), pRight)
		;
	}
	
	bool CAppDistributionManagerActor::CVersionManagerVersion::CCompareApplication::operator()(CVersionManager::CVersionIDAndPlatform const &_Left, CVersionManagerVersion const &_Right) const
	{
		return _Left < _Right.f_GetVersionID();
	}
	
	bool CAppDistributionManagerActor::CVersionManagerVersion::CCompareApplication::operator()(CVersionManagerVersion const &_Left, CVersionManager::CVersionIDAndPlatform const &_Right) const
	{
		return _Left.f_GetVersionID() < _Right;
	}

	CAppDistributionManagerActor::CVersionManagerApplication::CVersionManagerApplication(CAppDistributionManagerActor &_This)
		: m_This(_This)
	{
	}
	
	void CAppDistributionManagerActor::fp_VersionManagerSubscribe(CVersionManagerState &_VersionManagerState)
	{
		_VersionManagerState.m_Subscription.f_Clear();
		
		CVersionManager::CSubscribeToUpdates SubscriptionParams;
		SubscriptionParams.m_Application = CStr(); // All applications we have access to 
		SubscriptionParams.m_nInitial = 32;
		SubscriptionParams.m_DispatchActor = self;
		SubscriptionParams.m_fOnNewVersions
			= [this, Actor = _VersionManagerState.f_GetManager().f_Weak(), AllowDestroy = g_AllowWrongThreadDestroy]
			(CVersionManager::CNewVersionNotifications &&_NewVersions) 
			-> NConcurrency::TCFuture<CVersionManager::CNewVersionNotifications::CResult>
			{
				auto *pManager = mp_VersionManagers.f_FindEqual(Actor);
				if (!pManager)
					return DMibErrorInstance("Manager gone");
				auto &Manager = *pManager;

				if (_NewVersions.m_bFullResend)
					Manager.m_Versions.f_Clear();
				
				for (auto &NewVersion : _NewVersions.m_NewVersions)
				{
					auto &Version = *(Manager.m_Versions[NewVersion.m_Application](NewVersion.m_VersionIDAndPlatform, &Manager));
					Version.f_SetApplication(nullptr);
					auto &Application = *mp_VersionManagerApplications(NewVersion.m_Application, *this);
					Version.m_VersionInfo = fg_Move(NewVersion.m_VersionInfo);
					Version.f_SetApplication(&Application);
				}
				
				fp_AutoUpdate_Update();

				return fg_Explicit(CVersionManager::CNewVersionNotifications::CResult());
			}
		;
		
		mint SubscribeSequence = ++_VersionManagerState.m_SubscribeSequence;
		
		DCallActor(_VersionManagerState.f_GetManager(), CVersionManager::f_SubscribeToUpdates, fg_Move(SubscriptionParams))
			> [this, SubscribeSequence, Actor = _VersionManagerState.f_GetManager().f_Weak()](TCAsyncResult<CVersionManager::CSubscribeToUpdates::CResult> &&_Result)
			{
				auto *pManager = mp_VersionManagers.f_FindEqual(Actor); 
				if (!_Result)
				{
					CStr ErrorMessage = fg_Format("Error subscribing to version updates: {}", _Result.f_GetExceptionStr());
					if (pManager)
					{
						DMibLogWithCategory(Malterlib/Cloud/AppDistributionManager, Error, "<{}> {}", pManager->m_HostInfo.m_HostInfo, ErrorMessage);
						mp_VersionManagers.f_Remove(pManager);
					}
					else
						DMibLogWithCategory(Malterlib/Cloud/AppDistributionManager, Error, "{}", ErrorMessage);
					return;
				}
				if (!pManager)
					return;
				
				if (SubscribeSequence != pManager->m_SubscribeSequence)
					return;
				
				auto &Manager = *pManager;
				Manager.m_Subscription = fg_Move(_Result->m_Subscription);
			}
		;
	}
	
	void CAppDistributionManagerActor::fp_VersionManagerResubscribeAll()
	{
		for (auto &Manager : mp_VersionManagers)
			fp_VersionManagerSubscribe(Manager);
	}
	
	void CAppDistributionManagerActor::fp_VersionManagerAdded(TCDistributedActor<CVersionManager> const &_VersionManager, CTrustedActorInfo const &_Info)
	{
		auto &NewManager = mp_VersionManagers[_VersionManager];
		NewManager.m_HostInfo = _Info;
		fp_VersionManagerSubscribe(NewManager);
	}
	
	void CAppDistributionManagerActor::fp_VersionManagerRemoved(TCWeakDistributedActor<CActor> const &_VersionManager)
	{
		mp_VersionManagers.f_Remove(_VersionManager);
	}
	
	
	auto CAppDistributionManagerActor::fp_DownloadApplicationFromManager
		(
			TCDistributedActor<CVersionManager> const &_Manager
			, CStr const &_ApplicationName
			, CVersionManager::CVersionIDAndPlatform const &_VersionID
			, CStr const &_DestinationDir
		)
		-> TCFuture<CVersionInformation>
	{
		auto &DownloadState = mp_Downloads.f_Insert();
		auto pDownloadState = &DownloadState;
		auto pCleanup = g_OnScopeExitActor > [this, pDownloadState]
			{
				mp_Downloads.f_Remove(*pDownloadState);
			}
		;
		TCPromise<CVersionInformation> Promise;

		DownloadState.m_DownloadVersionReceive = fg_ConstructActor<CFileTransferReceive>
			(
			 	_DestinationDir
			 	, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_GroupRead | EFileAttrib_EveryoneRead | EFileAttrib_UnixAttributesValid
			 	, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_GroupRead | EFileAttrib_EveryoneRead | EFileAttrib_UnixAttributesValid
			)
		;
		DownloadState.m_DownloadVersionReceive(&CFileTransferReceive::f_ReceiveFiles, 16*1024*1024, CFileTransferReceive::EReceiveFlag_FailOnExisting)
			> Promise % "Failed to initialize file transfer context" 
			/ [=]
			(CFileTransferContext &&_TransferContext)
			{
				CVersionManager::CStartDownloadVersion StartDownload;
				StartDownload.m_Application = _ApplicationName;
				StartDownload.m_VersionIDAndPlatform = _VersionID;
				StartDownload.m_TransferContext = fg_Move(_TransferContext);
				
				DMibCallActor
					(
						_Manager
						, CVersionManager::f_DownloadVersion
						, fg_Move(StartDownload)
					)
					.f_Timeout(30.0, "Timed out waiting for version manager to reply")
					> Promise % "Failed to start download on remote server" / [=]
					(CVersionManager::CStartDownloadVersion::CResult &&_Result)
					{
						pDownloadState->m_Subscription = fg_Move(_Result.m_Subscription);
						
						pDownloadState->m_DownloadVersionReceive(&CFileTransferReceive::f_GetResult) 
							> [=, VersionInfo = fg_Move(_Result.m_VersionInfo)]
							(TCAsyncResult<CFileTransferResult> &&_Results) mutable
							{
								pDownloadState->m_Subscription.f_Clear();
								if (!_Results)
									Promise.f_SetException(fg_Move(_Results));
								else
								{
									g_Dispatch(mp_FileActor) / [=]() -> TCVector<CStr>
										{
											TCVector<CStr> RawFiles = CFile::fs_FindFiles(_DestinationDir / "*");
											TCVector<CStr> Files;
											for (auto &File : RawFiles)
												Files.f_Insert(CFile::fs_MakePathRelative(File, _DestinationDir));

											return Files;
										}
										> Promise / [=](TCVector<CStr> &&_Files)
										{
											(void)pCleanup;
											Promise.f_SetResult(CVersionInformation{VersionInfo, fg_Move(_Files)});
										}
									;
								}
							}
						;
					}
				;
			}
		;
		
		return Promise.f_MoveFuture();
	}
	
	auto CAppDistributionManagerActor::fp_DownloadApplication
		(
			CStr const &_ApplicationName
			, CVersionManager::CVersionIDAndPlatform const &_VersionID
			, CStr const &_DestinationDir
		)
		-> TCFuture<CVersionInformation>
	{
		TCSet<TCDistributedActor<CVersionManager>> ManagersToTrySet;
		struct CState
		{
			TCLinkedList<TCDistributedActor<CVersionManager>> m_ManagersToTry;
			TCFunctionMovable<void (TCSharedPointer<CState> const &_pState)> m_fContinue;
			CStr m_Errors;
		};
		TCSharedPointer<CState> pState = fg_Construct();
		
		auto fAddManager = [&](TCDistributedActor<CVersionManager> const &_Manager)
			{
				if (!ManagersToTrySet(_Manager).f_WasCreated())
					return;
				pState->m_ManagersToTry.f_Insert(_Manager);
			}
		;
		auto *pApplication = mp_VersionManagerApplications.f_FindEqual(_ApplicationName);
		if (pApplication)
		{
			
			auto *pVersion = pApplication->m_Versions.f_FindEqual(_VersionID);
			if (pVersion)
				fAddManager(pVersion->m_pVersionManager->f_GetManager());
			for (auto &Version : pApplication->m_Versions)
				fAddManager(Version.m_pVersionManager->f_GetManager());
			if (ManagersToTrySet.f_IsEmpty())
				return DMibErrorInstance("Found no version managers with this application on");
		}
		else
		{
			for (auto &VersionManager : mp_VersionManagers)
				fAddManager(VersionManager.f_GetManager());
			if (ManagersToTrySet.f_IsEmpty())
				return DMibErrorInstance("Found no version managers to check if application exists on");
		}
		
		TCPromise<CVersionInformation> Promise;
		
		pState->m_fContinue = [this, Promise, _ApplicationName, _VersionID, _DestinationDir](TCSharedPointer<CState> const &_pState)
			{
				if (_pState->m_ManagersToTry.f_IsEmpty())
				{
					Promise.f_SetException(DMibErrorInstance(fg_Format("Failed to download the application from any manager: {}", _pState->m_Errors)));
					return;
				}
				auto Manager = _pState->m_ManagersToTry.f_Pop();
				self(&CAppDistributionManagerActor::fp_DownloadApplicationFromManager, Manager, _ApplicationName, _VersionID, _DestinationDir) 
					> [_pState, Promise](TCAsyncResult<CVersionInformation> &&_Result)
					{
						if (!_Result)
						{
							fg_AddStrSep(_pState->m_Errors, _Result.f_GetExceptionStr(), "\n"); 
							_pState->m_fContinue(_pState);
							return;
						}
						Promise.f_SetResult(fg_Move(*_Result));
					}
				;
			}
		;
		
		pState->m_fContinue(pState);
		
		return Promise.f_MoveFuture();
	}
}
