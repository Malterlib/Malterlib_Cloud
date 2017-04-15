// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Process/Platform>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"
#include "Malterlib_Cloud_BackupManagerClient_BackupInstance.h"

namespace NMib::NCloud
{
	CBackupManagerClient::CBackupManagerClient(CConfig const &_Config, NConcurrency::TCActor<NConcurrency::CDistributedActorTrustManager> const &_TrustManager)
		: mp_pInternal(fg_Construct(this, _Config.f_Validate(), _TrustManager)) 
	{
		auto &Internal = *mp_pInternal;
		
		Internal.f_Construct();
		Internal.f_RunBackup();
	}
	
	CBackupManagerClient::~CBackupManagerClient() = default;

	NConcurrency::TCContinuation<void> CBackupManagerClient::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;
		*Internal.m_pDestroyed = true;
		
		TCActorResultVector<void> Destroys;
		for (auto &BackupInstance : Internal.m_RunningBackupInstances)
			BackupInstance->f_Destroy() > Destroys.f_AddResult();
		
		Internal.m_RunningBackupInstances.f_Clear();
		
		auto pTracker = fg_Move(Internal.m_pCanDestroyTracker);
		
		pTracker->m_Continuation.f_Dispatch() > Destroys.f_AddResult();  
		
		NConcurrency::TCContinuation<void> Continuation;
		Destroys.f_GetResults() > Continuation.f_ReceiveAny();
		
		return Continuation;
	}
	
	void CBackupManagerClient::CInternal::fs_CheckDestroy(TCSharedPointer<NAtomic::TCAtomic<bool>> const &_pDestroyed)
	{
		if (_pDestroyed->f_Load(NAtomic::EMemoryOrder_Relaxed))
			DMibError("Backup client destroyed");
	}
	
	void CBackupManagerClient::CInternal::f_Construct()
	{
		m_FileActor = fg_Construct(fg_Construct(), "BackupManagerClient file actor");
		m_FileChangeNotificationsActor = fg_Construct();
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
					DMibLogCategoryStr(m_Config.m_LogCategory);
					DMibLog(Error, "Failed to subscribe to file notifications: {}", _Result.f_GetExceptionStr());
				}
				g_Dispatch(m_FileActor) > [Config = m_Config.m_ManifestConfig, pDestroyed = m_pDestroyed]
					{
						return CDirectoryManifest::fs_GetManifest(Config, [&]{ fs_CheckDestroy(pDestroyed); });
					}
					> [this](TCAsyncResult<CDirectoryManifest> &&_Manifest)
					{
						if (m_pThis->mp_bDestroyed)
							return;
						
						if (!_Manifest)
						{
							DMibLogCategoryStr(m_Config.m_LogCategory);
							DMibLog(Error, "Failed to get manifest: {}", _Manifest.f_GetExceptionStr());
							return;
						}
						
						m_Manifest = fg_Move(*_Manifest);

						f_NewBackupKey();
						f_Subscribe();
					}
				;
			}
		;
	}
	
	auto CBackupManagerClient::CConfig::f_Validate() const -> CConfig const &
	{
		m_ManifestConfig.f_Validate();
		return *this;
	}
}
