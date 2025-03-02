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

	TCFuture<void> CAppDistributionManagerActor::fp_VersionManagerSubscribe(TCWeakDistributedActor<CVersionManager> _VersionManager)
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
			(CVersionManager::CNewVersionNotifications _NewVersions)
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

	TCFuture<void> CAppDistributionManagerActor::fp_VersionManagerAdded(TCDistributedActor<CVersionManager> _VersionManager, CTrustedActorInfo _Info)
	{
		auto &NewManager = mp_VersionManagers[_VersionManager];
		NewManager.m_HostInfo = _Info;
		co_await fp_VersionManagerSubscribe(_VersionManager.f_Weak());

		co_return {};
	}

	TCFuture<void> CAppDistributionManagerActor::fp_VersionManagerRemoved(TCWeakDistributedActor<CActor> _VersionManager)
	{
		mp_VersionManagers.f_Remove(_VersionManager);

		co_return {};
	}

	auto CAppDistributionManagerActor::fp_DownloadApplicationFromManager
		(
			TCDistributedActor<CVersionManager> _Manager
			, CStr _ApplicationName
			, CVersionManager::CVersionIDAndPlatform _VersionID
			, CStr _DestinationDir
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
		DownloadState.m_DownloadVersionReceive = fg_ConstructActor<CFileTransferReceive>
			(
				_DestinationDir
				, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_GroupRead | EFileAttrib_EveryoneRead | EFileAttrib_UnixAttributesValid
				, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_GroupRead | EFileAttrib_EveryoneRead | EFileAttrib_UnixAttributesValid
			)
		;

		if (_Manager->f_InterfaceVersion() >= CVersionManager::EProtocolVersion_AsyncGeneratorFileTransfer)
		{
			CVersionManager::CStartDownloadVersion StartDownload;
			StartDownload.m_Application = _ApplicationName;
			StartDownload.m_VersionIDAndPlatform = _VersionID;
			StartDownload.m_Subscription = co_await DownloadState.m_DownloadVersionReceive.f_Bind<&CFileTransferReceive::f_GetAbortSubscription>();

			auto Result = co_await
				(
					_Manager.f_CallActor(&CVersionManager::f_DownloadVersion)(fg_Move(StartDownload))
					.f_Timeout(30.0, "Timed out waiting for version manager to reply")
					% "Failed to start download on remote server"
				)
			;

			if (!Result.m_FilesGenerator)
				co_return DMibErrorInstance("Internal Error: No files generator");

			pDownloadState->m_Subscription = fg_Move(Result.m_Subscription);

			auto Results = co_await
				(
					DownloadState.m_DownloadVersionReceive
					(
						&CFileTransferReceive::f_ReceiveFiles
						, CFileTransferSendDownloadFile::fs_TranslateGenerator<CFileTransferSendDownloadFile>(fg_Move(*Result.m_FilesGenerator))
						, NFile::gc_IdealNetworkQueueSize
						, CFileTransferReceive::EReceiveFlag_FailOnExisting
					)
					% "Failed to receive files"
				)
				.f_Wrap()
			;

			if (pDownloadState->m_Subscription)
				(void)co_await fg_Exchange(pDownloadState->m_Subscription, nullptr)->f_Destroy().f_Wrap();

			if (!Results)
				co_return fg_Move(Results).f_GetException();

			auto BlockingActorCheckout = fg_BlockingActor();
			auto Files = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [=]() -> TCVector<CStr>
					{
						TCVector<CStr> RawFiles = CFile::fs_FindFiles(_DestinationDir / "*");
						TCVector<CStr> Files;
						for (auto &File : RawFiles)
							Files.f_Insert(CFile::fs_MakePathRelative(File, _DestinationDir));

						return Files;
					}
				)
			;

			co_return CVersionInformation{fg_Move(Result.m_VersionInfo), fg_Move(Files)};
		}
		else
		{
			auto TransferContext = co_await
				(
					DownloadState.m_DownloadVersionReceive
					(
						&CFileTransferReceive::f_ReceiveFilesDeprecated
						, NFile::gc_IdealNetworkQueueSize
						, CFileTransferReceive::EReceiveFlag_FailOnExisting
					)
					% "Failed to initialize file transfer context"
				)
			;

			CVersionManager::CStartDownloadVersion StartDownload;
			StartDownload.m_Application = _ApplicationName;
			StartDownload.m_VersionIDAndPlatform = _VersionID;
			StartDownload.m_TransferContextDeprecated = fg_Move(TransferContext);

			if (_Manager->f_InterfaceVersion() >= CVersionManager::EProtocolVersion_RefactorToActorFunctorsUploadDownload)
				StartDownload.m_Subscription = co_await DownloadState.m_DownloadVersionReceive.f_Bind<&CFileTransferReceive::f_GetAbortSubscription>();

			auto Result = co_await
				(
					_Manager.f_CallActor(&CVersionManager::f_DownloadVersion)(fg_Move(StartDownload))
					.f_Timeout(30.0, "Timed out waiting for version manager to reply")
					% "Failed to start download on remote server"
				)
			;

			pDownloadState->m_Subscription = fg_Move(Result.m_Subscription);

			auto Results = co_await pDownloadState->m_DownloadVersionReceive(&CFileTransferReceive::f_GetResultDeprecated).f_Wrap();

			if (pDownloadState->m_Subscription)
				(void)co_await fg_Exchange(pDownloadState->m_Subscription, nullptr)->f_Destroy().f_Wrap();

			if (!Results)
				co_return fg_Move(Results).f_GetException();

			auto BlockingActorCheckout = fg_BlockingActor();
			auto Files = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [=]() -> TCVector<CStr>
					{
						TCVector<CStr> RawFiles = CFile::fs_FindFiles(_DestinationDir / "*");
						TCVector<CStr> Files;
						for (auto &File : RawFiles)
							Files.f_Insert(CFile::fs_MakePathRelative(File, _DestinationDir));

						return Files;
					}
				)
			;

			co_return CVersionInformation{fg_Move(Result.m_VersionInfo), fg_Move(Files)};
		}
	}

	auto CAppDistributionManagerActor::fp_DownloadApplication
		(
			CStr _ApplicationName
			, CVersionManager::CVersionIDAndPlatform _VersionID
			, CStr _DestinationDir
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
				co_return DMibErrorInstance("Found no version managers with this application on");
		}
		else
		{
			for (auto &VersionManager : mp_VersionManagers)
				fAddManager(VersionManager.f_GetManager());
			if (ManagersToTrySet.f_IsEmpty())
				co_return DMibErrorInstance("Found no version managers to check if application exists on");
		}

		TCPromiseFuturePair<CVersionInformation> Promise;
		pState->m_fContinue = [this, Promise = fg_Move(Promise.m_Promise), _ApplicationName, _VersionID, _DestinationDir](TCSharedPointer<CState> const &_pState) mutable
			{
				if (_pState->m_ManagersToTry.f_IsEmpty())
				{
					Promise.f_SetException(DMibErrorInstance(fg_Format("Failed to download the application from any manager: {}", _pState->m_Errors)));
					return;
				}
				auto Manager = _pState->m_ManagersToTry.f_Pop();
				fp_DownloadApplicationFromManager(Manager, _ApplicationName, _VersionID, _DestinationDir)
					> [_pState, Promise = fg_Move(Promise)](TCAsyncResult<CVersionInformation> &&_Result)
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

		co_return co_await fg_Move(Promise.m_Future);
	}
}
