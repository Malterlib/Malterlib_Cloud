// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/Platform>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"
#include "Malterlib_Cloud_BackupManagerClient_BackupInstance.h"

namespace NMib::NCloud
{
	CBackupManagerClient::CBackupManagerClient
		(
			CConfig const &_Config
			, NConcurrency::TCActor<NConcurrency::CDistributedActorTrustManager> const &_TrustManager
			, TCActorFunctor
			<
				TCFuture<TCActorSubscriptionWithID<>>
				(
					TCDistributedActorInterfaceWithID<CDistributedAppInterfaceBackup> &&_BackupInterface
					, CActorSubscription &&_ManifestFinished
					, CStr const &_BackupRoot
				)
			>
			&&_fOnNewBackup
			, TCActor<CActorDistributionManager> const &_DistributionManager
		)
		: mp_pInternal(fg_Construct(this, _Config, _TrustManager, fg_Move(_fOnNewBackup)))
	{
		auto &Internal = *mp_pInternal;
		
		Internal.f_Construct(_DistributionManager);
	}

	CBackupManagerClient::~CBackupManagerClient() = default;

	NConcurrency::TCFuture<void> CBackupManagerClient::f_StartBackup()
	{
		auto &Internal = *mp_pInternal;
		if (Internal.m_bStarted)
			return DMibErrorInstance("Backup already started");

		try
		{
			Internal.m_Config.f_Validate();
		}
		catch (NException::CException const &_Exception)
		{
			return _Exception;
		}

		Internal.m_bStarted = true;

		Internal.f_RunBackup();

		return fg_Explicit();
	}


	NConcurrency::TCFuture<void> CBackupManagerClient::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;
		*Internal.m_pDestroyed = true;
		{
			TCActorResultVector<void> RunningInstancesDestroys;
			for (auto &BackupInstance : Internal.m_RunningBackupInstances)
				BackupInstance.m_Instance->f_Destroy() > RunningInstancesDestroys.f_AddResult();

			Internal.m_RunningBackupInstances.f_Clear();

			co_await RunningInstancesDestroys.f_GetResults().f_Wrap();
		}
		{
			TCActorResultVector<void> StoppedNotifications;
			for (auto &fOnStopped : Internal.m_OnBackupStoppedSubscriptions)
				fOnStopped() > StoppedNotifications.f_AddResult();

			co_await StoppedNotifications.f_GetResults().f_Wrap();
		}
		{
			TCActorResultVector<void> Destroys;

			Internal.m_fOnNewBackup.f_Destroy() > Destroys.f_AddResult();

			{
				auto pTracker = fg_Move(Internal.m_pCanDestroyTracker);
				pTracker->f_Future() > Destroys.f_AddResult();
			}
			co_await Destroys.f_GetResults().f_Wrap();
		}
		co_await
			(
				g_Dispatch(Internal.m_FileActor) / [AppendStates = fg_Move(Internal.m_AppendStates)]
				{
				}
			)
		;
		co_await Internal.m_FileActor->f_Destroy();
		co_await Internal.m_BackupInterface.f_Destroy();

		co_return {};
	}

	void CBackupManagerClient::CInternal::fs_CheckDestroy(TCSharedPointer<NAtomic::TCAtomic<bool>> const &_pDestroyed)
	{
		if (_pDestroyed->f_Load(NAtomic::EMemoryOrder_Relaxed))
			DMibError("Backup client destroyed");
	}

	void CBackupManagerClient::CInternal::f_Construct(TCActor<CActorDistributionManager> const &_DistributionManager)
	{
		m_FileActor = fg_Construct(fg_Construct(), "BackupManagerClient file actor");
		m_FileChangeNotificationsActor = fg_Construct();
		if (_DistributionManager)
			m_BackupInterface.f_Construct(_DistributionManager, m_pThis);
	}

	void CBackupManagerClient::CInternal::f_NewBackupKey()
	{
		m_BackupKey.m_Time = NTime::CTime::fs_NowUTC();
		m_BackupKey.m_ID = NCryptography::fg_RandomID();
		if (m_Config.m_BackupIdentifier.f_IsEmpty())
			m_BackupKey.m_FriendlyName = NProcess::NPlatform::fg_Process_GetComputerName();
		else
			m_BackupKey.m_FriendlyName = fg_Format("{}-{}", NProcess::NPlatform::fg_Process_GetComputerName(), m_Config.m_BackupIdentifier);
	}

	void CBackupManagerClient::CInternal::f_RunBackup()
	{
		f_SubscribeChanges() > [this](TCAsyncResult<void> &&_Result)
			{
				if (m_pThis->mp_bDestroyed)
					return;

				if (!_Result)
				{
					f_ReportBackupError("Failed to subscribe to file notifications: {}"_f << _Result.f_GetExceptionStr(), true);
					return;
				}

				m_bInitialSubscribeDone = true;

				g_Dispatch(m_FileActor) / [Config = m_Config.m_ManifestConfig, pDestroyed = m_pDestroyed]()
					-> TCTuple<CDirectoryManifest, TCMap<CStr, CUniqueFileIdentifier>, TCMap<CStr, TCSharedPointer<CAppendFileState>>>
					{
						TCMap<CStr, CFile::CFileChecksumState_SHA256> SourceAppendStates;
						
						auto Manifest = CDirectoryManifest::fs_GetManifest(Config, [&]{ fs_CheckDestroy(pDestroyed); }, &SourceAppendStates, gc_ChecksumFileFlags);
						Manifest.m_Files.f_Remove("");
						TCMap<CStr, CUniqueFileIdentifier> FileIDs;
						TCMap<CStr, TCSharedPointer<CAppendFileState>> AppendStates;
						
						for (auto &File : Manifest.m_Files)
						{
							if (File.m_Attributes & (EFileAttrib_Link | EFileAttrib_Directory))
								continue;

							auto &FileName = Manifest.m_Files.fs_GetKey(File);

							auto AbsolutePath = CFile::fs_AppendPath(Config.m_Root, File.m_OriginalPath);
							FileIDs[FileName] = CFile::fs_GetUniqueIdentifier(AbsolutePath);
							
							if (auto *pSourceAppendState = SourceAppendStates.f_FindEqual(FileName))
							{
								auto &AppendState = *(AppendStates[FileName] = fg_Construct());
								AppendState.m_ChecksumState.m_DigestState = pSourceAppendState->m_Hash;
								AppendState.m_File = fg_Move(*pSourceAppendState->m_pFile);
								AppendState.m_ManifestFile = File;
								AppendState.m_bIsValid = true;
								AppendState.m_ChecksumState.m_Position = AppendState.m_File.f_GetPosition();
							}
						}

						return {fg_Move(Manifest), fg_Move(FileIDs), fg_Move(AppendStates)};
					}
					> [this](TCAsyncResult<TCTuple<CDirectoryManifest, TCMap<CStr, CUniqueFileIdentifier>, TCMap<CStr, TCSharedPointer<CAppendFileState>>>> &&_Manifest)
					{
						if (m_pThis->mp_bDestroyed)
							return;

						if (!_Manifest)
						{
							f_ReportBackupError("Failed to get manifest: {}"_f << _Manifest.f_GetExceptionStr(), true);
							return;
						}

						m_Manifest = fg_Move(fg_Get<0>(*_Manifest));
						m_ManifestFileIDs = fg_Move(fg_Get<1>(*_Manifest));
						m_AppendStates = fg_Move(fg_Get<2>(*_Manifest));

						for (auto &pAppendState : m_AppendStates)
							m_ChecksumState[m_AppendStates.fs_GetKey(pAppendState)] = pAppendState->m_ChecksumState;

						f_NewBackupKey();
						
						if (m_fOnNewBackup)
						{
							m_fOnNewBackup
								(
									m_BackupInterface.m_Actor->f_ShareInterface<CDistributedAppInterfaceBackup>()
									, g_ActorSubscription / [this]
									{
										f_BackupFinishedStarting();
									}
									, m_Config.m_ManifestConfig.m_Root
								)
								> [this](TCAsyncResult<TCActorSubscriptionWithID<>> &&_Result)
								{
									if (!_Result)
									{
										DMibLogCategoryStr(m_Config.m_LogCategory);
										DMibLog(Error, "Failed run on new backup: {}", _Result.f_GetExceptionStr());
									}
									else
										m_BackupInterfaceSubscription = fg_Move(*_Result);
								}
							;
						}
						else
							f_BackupFinishedStarting();
						
						f_Subscribe();
					}
				;
			}
		;
	}

	auto CBackupManagerClient::CConfig::f_Validate() const -> CConfig const &
	{
		m_ManifestConfig.f_Validate();
		if (m_MaxSendQueue < 16*1024)
			DMibError("Send queue needs to be at least 16 KiB");
		return *this;
	}
}
