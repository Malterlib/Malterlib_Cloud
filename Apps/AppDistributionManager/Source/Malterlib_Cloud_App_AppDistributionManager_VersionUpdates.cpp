// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
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
			if (m_pApplication->m_VersionsByTime.f_IsEmpty() && m_pApplication->m_Versions.f_IsEmpty())
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

	COrdering_Partial CAppDistributionManagerActor::CVersionManagerVersion::CCompareApplicationByTime::operator()
	(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const
	{
		auto *pLeft = &_Left;
		auto *pRight = &_Right;
		if (auto Result = _Left.m_VersionInfo.m_Time.f_IsValid() <=> _Right.m_VersionInfo.m_Time.f_IsValid(); Result != 0)
			return Result;

		return NStorage::fg_TupleReferences(_Left.m_VersionInfo.m_Time, _Left.f_GetVersionID(), pLeft)
			<=> NStorage::fg_TupleReferences(_Right.m_VersionInfo.m_Time, _Right.f_GetVersionID(), pRight)
		;
	}

	COrdering_Partial CAppDistributionManagerActor::CVersionManagerVersion::CCompareApplication::operator()
	(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const
	{
		auto *pLeft = &_Left;
		auto *pRight = &_Right;
		return NStorage::fg_TupleReferences(_Left.f_GetVersionID(), pLeft)
			<=> NStorage::fg_TupleReferences(_Right.f_GetVersionID(), pRight)
		;
	}

	COrdering_Partial CAppDistributionManagerActor::CVersionManagerVersion::CCompareApplication::operator()
	(CVersionManager::CVersionIDAndPlatform const &_Left, CVersionManagerVersion const &_Right) const
	{
		return _Left <=> _Right.f_GetVersionID();
	}

	COrdering_Partial CAppDistributionManagerActor::CVersionManagerVersion::CCompareApplication::operator()
	(CVersionManagerVersion const &_Left, CVersionManager::CVersionIDAndPlatform const &_Right) const
	{
		return _Left.f_GetVersionID() <=> _Right;
	}

	CAppDistributionManagerActor::CVersionManagerApplication::CVersionManagerApplication(CAppDistributionManagerActor &_This)
		: m_This(_This)
	{
	}

	TCFuture<void> CAppDistributionManagerActor::fp_VersionManagerSubscribe(TCWeakDistributedActor<CVersionManager> const &_VersionManager)
	{
		CVersionManagerState *pVersionManagerState;

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> NException::CExceptionPointer
				{
					pVersionManagerState = mp_VersionManagers.f_FindEqual(_VersionManager);
					if (!pVersionManagerState)
						return DMibErrorInstance("Version manager removed");
					return {};
				}
			)
		;

		pVersionManagerState->m_Subscription.f_Clear();

		CVersionManager::CSubscribeToUpdates SubscriptionParams;
		SubscriptionParams.m_Application = CStr(); // All applications we have access to
		SubscriptionParams.m_nInitial = 32;
		SubscriptionParams.m_fOnNewVersions = g_ActorFunctor / [this, Actor = pVersionManagerState->f_GetManager().f_Weak(), AllowDestroy = g_AllowWrongThreadDestroy]
			(CVersionManager::CNewVersionNotifications &&_NewVersions)
			-> NConcurrency::TCFuture<CVersionManager::CNewVersionNotifications::CResult>
			{
				auto *pManager = mp_VersionManagers.f_FindEqual(Actor);
				if (!pManager)
					co_return DMibErrorInstance("Manager gone");
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

				co_return CVersionManager::CNewVersionNotifications::CResult();
			}
		;

		mint SubscribeSequence = ++pVersionManagerState->m_SubscribeSequence;

		auto Result = co_await pVersionManagerState->f_GetManager().f_CallActor(&CVersionManager::f_SubscribeToUpdates)(fg_Move(SubscriptionParams)).f_Wrap();

		if (!Result)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppDistributionManager, Error, "Error subscribing to version updates: {}", Result.f_GetExceptionStr());
			co_return {};
		}

		if (SubscribeSequence != pVersionManagerState->m_SubscribeSequence)
			co_return {};

		pVersionManagerState->m_Subscription = fg_Move(Result->m_Subscription);
		co_return {};
	}

	TCFuture<void> CAppDistributionManagerActor::fp_VersionManagerAdded(TCDistributedActor<CVersionManager> const &_VersionManager, CTrustedActorInfo const &_Info)
	{
		auto &NewManager = mp_VersionManagers[_VersionManager];
		NewManager.m_HostInfo = _Info;
		co_await self(&CAppDistributionManagerActor::fp_VersionManagerSubscribe, _VersionManager.f_Weak());

		co_return {};
	}

	TCFuture<void> CAppDistributionManagerActor::fp_VersionManagerRemoved(TCWeakDistributedActor<CActor> const &_VersionManager)
	{
		mp_VersionManagers.f_Remove(_VersionManager);

		co_return {};
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
		auto pCleanup = g_OnScopeExitActor / [this, pDownloadState]
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
			/ [=, this]
			(CFileTransferContext &&_TransferContext)
			{
				CVersionManager::CStartDownloadVersion StartDownload;
				StartDownload.m_Application = _ApplicationName;
				StartDownload.m_VersionIDAndPlatform = _VersionID;
				StartDownload.m_TransferContext = fg_Move(_TransferContext);

				_Manager.f_CallActor(&CVersionManager::f_DownloadVersion)(fg_Move(StartDownload))
					.f_Timeout(30.0, "Timed out waiting for version manager to reply")
					> Promise % "Failed to start download on remote server" / [=, this]
					(CVersionManager::CStartDownloadVersion::CResult &&_Result)
					{
						pDownloadState->m_Subscription = fg_Move(_Result.m_Subscription);

						pDownloadState->m_DownloadVersionReceive(&CFileTransferReceive::f_GetResult)
							> [=, this, VersionInfo = fg_Move(_Result.m_VersionInfo)]
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
		TCPromise<CVersionInformation> Promise;

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
				return Promise <<= DMibErrorInstance("Found no version managers with this application on");
		}
		else
		{
			for (auto &VersionManager : mp_VersionManagers)
				fAddManager(VersionManager.f_GetManager());
			if (ManagersToTrySet.f_IsEmpty())
				return Promise <<= DMibErrorInstance("Found no version managers to check if application exists on");
		}

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
