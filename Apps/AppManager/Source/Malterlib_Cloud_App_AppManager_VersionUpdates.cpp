// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/ActorSubscription>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	CAppManagerActor::CVersionManagerVersion::CVersionManagerVersion(CVersionManagerState *_pVersionManager)
		: m_pVersionManager(_pVersionManager)
	{
	}

	CAppManagerActor::CVersionManagerVersion::~CVersionManagerVersion()
	{
		f_SetApplication(nullptr);
	}

	void CAppManagerActor::CVersionManagerVersion::f_SetApplication(CVersionManagerApplication *_pApplication)
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

	CVersionManager::CVersionIDAndPlatform const &CAppManagerActor::CVersionManagerVersion::f_GetVersionID() const
	{
		return TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManagerVersion>::fs_GetKey(*this);
	}

	COrdering_Partial CAppManagerActor::CVersionManagerVersion::CCompareApplicationByTime::operator()(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const
	{
		auto *pLeft = &_Left;
		auto *pRight = &_Right;

		if (auto Result = _Left.m_VersionInfo.m_Time.f_IsValid() <=> _Right.m_VersionInfo.m_Time.f_IsValid(); Result != 0)
			return Result;

		return NStorage::fg_TupleReferences(_Left.m_VersionInfo.m_Time, _Left.f_GetVersionID(), pLeft)
			<=> NStorage::fg_TupleReferences(_Right.m_VersionInfo.m_Time, _Right.f_GetVersionID(), pRight)
		;
	}

	COrdering_Partial CAppManagerActor::CVersionManagerVersion::CCompareApplication::operator()(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const
	{
		auto *pLeft = &_Left;
		auto *pRight = &_Right;

		return NStorage::fg_TupleReferences(_Left.f_GetVersionID(), pLeft) <=> NStorage::fg_TupleReferences(_Right.f_GetVersionID(), pRight);
	}

	COrdering_Partial CAppManagerActor::CVersionManagerVersion::CCompareApplication::operator()
	(CVersionManager::CVersionIDAndPlatform const &_Left, CVersionManagerVersion const &_Right) const
	{
		return _Left <=> _Right.f_GetVersionID();
	}

	COrdering_Partial CAppManagerActor::CVersionManagerVersion::CCompareApplication::operator()
	(CVersionManagerVersion const &_Left, CVersionManager::CVersionIDAndPlatform const &_Right) const
	{
		return _Left.f_GetVersionID() <=> _Right;
	}

	CAppManagerActor::CVersionManagerApplication::CVersionManagerApplication(CAppManagerActor &_This)
		: m_This(_This)
	{
	}

	TCFuture<void> CAppManagerActor::fp_VersionManagerSubscribe(CVersionManagerState &_VersionManagerState)
	{
		co_await ECoroutineFlag_AllowReferences;

		_VersionManagerState.m_Subscription.f_Clear();

		CVersionManager::CSubscribeToUpdates SubscriptionParams;
		SubscriptionParams.m_Application = CStr(); // All applications we have access to
		SubscriptionParams.m_nInitial = 20;
		SubscriptionParams.m_Platforms = mp_KnownPlatforms;
		SubscriptionParams.m_fOnNewVersions = g_ActorFunctor / [this, Actor = _VersionManagerState.f_GetManager().f_Weak(), AllowDestroy = g_AllowWrongThreadDestroy]
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

				if (_NewVersions.m_bFullResend && mp_AutoUpdateDelay >= 0.01)
					co_await fg_Timeout(mp_AutoUpdateDelay); // Wait for other managers to send their versions

				fp_AutoUpdate_Update();

				co_return {};
			}
		;

		mint SubscribeSequence = ++_VersionManagerState.m_SubscribeSequence;

		auto Actor = _VersionManagerState.f_GetManager().f_Weak();
		auto Subscription = co_await _VersionManagerState.f_GetManager().f_CallActor(&CVersionManager::f_SubscribeToUpdates)(fg_Move(SubscriptionParams)).f_Wrap();
		auto *pManager = mp_VersionManagers.f_FindEqual(Actor);
		if (!Subscription)
		{
			CStr ErrorMessage = fg_Format("Error subscribing to version updates: {}", Subscription.f_GetExceptionStr());
			if (pManager)
			{
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "<{}> {}", pManager->m_HostInfo.m_HostInfo, ErrorMessage);
				mp_VersionManagers.f_Remove(pManager);
			}
			else
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "{}", ErrorMessage);
			co_return {};
		}

		if (!pManager)
			co_return {};

		if (SubscribeSequence != pManager->m_SubscribeSequence)
			co_return{};

		auto &Manager = *pManager;
		Manager.m_Subscription = fg_Move(Subscription->m_Subscription);

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_VersionManagerResubscribeAll()
	{
		TCActorResultVector<void> Results;
		for (auto &Manager : mp_VersionManagers)
			fp_VersionManagerSubscribe(Manager) > Results.f_AddResult();

		co_await (co_await Results.f_GetResults() | g_Unwrap);

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_VersionManagerAdded(TCDistributedActor<CVersionManager> const &_VersionManager, CTrustedActorInfo const &_Info)
	{
		auto &NewManager = mp_VersionManagers[_VersionManager];
		NewManager.m_HostInfo = _Info;
		co_await fp_VersionManagerSubscribe(NewManager);

		co_return {};
	}

	void CAppManagerActor::fp_VersionManagerRemoved(TCWeakDistributedActor<CActor> const &_VersionManager)
	{
		mp_VersionManagers.f_Remove(_VersionManager);
	}

	TCFuture<CVersionManager::CVersionInformation> CAppManagerActor::fp_DownloadApplicationFromManager
		(
			TCDistributedActor<CVersionManager> const &_Manager
			, CStr const &_ApplicationName
			, CVersionManager::CVersionIDAndPlatform const &_VersionID
			, CStr const &_DestinationDir
		)
	{
		using namespace NStr;

		TCPromise<CVersionManager::CVersionInformation> Promise;
		
		auto &DownloadState = mp_Downloads.f_Insert();
		auto pDownloadState = &DownloadState;
		auto pCleanup = g_OnScopeExitActor / [this, pDownloadState]
			{
				mp_Downloads.f_Remove(*pDownloadState);
			}
		;

		DownloadState.m_DownloadVersionReceive = fg_ConstructActor<CFileTransferReceive>
			(
				_DestinationDir
				, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UnixAttributesValid
				, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UnixAttributesValid
			)
		;
		DownloadState.m_DownloadVersionReceive(&CFileTransferReceive::f_ReceiveFiles, 16*1024*1024, CFileTransferReceive::EReceiveFlag_DeleteExisting)
			> [_ApplicationName, _VersionID, _Manager, Promise, pDownloadState, pCleanup](TCAsyncResult<CFileTransferContext> &&_TransferContext)
			{
				if (!_TransferContext)
				{
					if (!Promise.f_IsSet())
						Promise.f_SetException(DMibErrorInstance("Failed to initialize file transfer context: {}"_f << _TransferContext.f_GetExceptionStr()));

					return;
				}

				auto &TransferContext = *_TransferContext;

				CVersionManager::CStartDownloadVersion StartDownload;
				StartDownload.m_Application = _ApplicationName;
				StartDownload.m_VersionIDAndPlatform = _VersionID;
				StartDownload.m_TransferContext = fg_Move(TransferContext);
				if (_Manager->f_InterfaceVersion() >= CVersionManager::EProtocolVersion_RefactorToActorFunctorsUploadDownload)
				{
					StartDownload.m_Subscription = g_ActorSubscription / [Promise]()
						{
							if (!Promise.f_IsSet())
								Promise.f_SetException(DMibErrorInstance("VersionManager disconnected"));
						}
					;
				}

				_Manager.f_CallActor(&CVersionManager::f_DownloadVersion)(fg_Move(StartDownload))
					.f_Timeout(60.0, "Timed out waiting for version manager to reply")
					> [Promise, pCleanup, pDownloadState](TCAsyncResult<CVersionManager::CStartDownloadVersion::CResult> &&_Result)
					{
						if (!_Result)
						{
							if (!Promise.f_IsSet())
								Promise.f_SetException(DMibErrorInstance("Failed to start download on remote server: {}"_f << _Result.f_GetExceptionStr()));

							return;
						}
						auto &Result = *_Result;

						pDownloadState->m_Subscription = fg_Move(Result.m_Subscription);

						pDownloadState->m_DownloadVersionReceive(&CFileTransferReceive::f_GetResult)
							> [Promise, pDownloadState, pCleanup, VersionInfo = fg_Move(Result.m_VersionInfo)]
							(TCAsyncResult<CFileTransferResult> &&_Results) mutable
							{
								pDownloadState->m_Subscription.f_Clear();
								if (!Promise.f_IsSet())
								{
									if (!_Results)
										Promise.f_SetException(fg_Move(_Results));
									else
										Promise.f_SetResult(fg_Move(VersionInfo));
								}
							}
						;
					}
				;
			}
		;

		return Promise.f_MoveFuture();
	}

	TCFuture<CVersionManager::CVersionInformation> CAppManagerActor::fp_DownloadApplication
		(
			CStr const &_ApplicationName
			, CVersionManager::CVersionIDAndPlatform const &_VersionID
			, CStr const &_DestinationDir
		)
	{
		TCPromise<CVersionManager::CVersionInformation> Promise;

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
				self(&CAppManagerActor::fp_DownloadApplicationFromManager, Manager, _ApplicationName, _VersionID, _DestinationDir)
					> [_pState, Promise](TCAsyncResult<CVersionManager::CVersionInformation> &&_Result)
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
