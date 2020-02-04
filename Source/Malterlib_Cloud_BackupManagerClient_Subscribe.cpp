// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"
#include "Malterlib_Cloud_BackupManagerClient_BackupInstance.h"

namespace NMib::NCloud
{
	void CBackupManagerClient::CInternal::f_Subscribe()
	{
		m_TrustManager(&CDistributedActorTrustManager::f_SubscribeTrustedActors<CBackupManager>, CBackupManager::mc_pDefaultNamespace, fg_ThisActor(m_pThis))
			> [this](TCAsyncResult<TCTrustedActorSubscription<CBackupManager>> &&_Subscription)
			{
				if (!_Subscription)
				{
					DMibLogCategoryStr(m_Config.m_LogCategory);
					DMibLog(Error, "Failed to subscribe to backup managers: {}", _Subscription.f_GetExceptionStr());
					return;
				}
				m_BackupManagers = fg_Move(*_Subscription);
				m_BackupManagers.f_OnActor
					(
						[this](TCDistributedActor<CBackupManager> const &_Actor, CTrustedActorInfo const &_ActorInfo)
						{
							if (m_pThis->f_IsDestroyed())
								return;
							auto &BackupInstance = m_RunningBackupInstances[_Actor].m_Instance = fg_Construct
								(
									fg_Construct(_Actor, m_Manifest, m_ChecksumState, m_Config, _ActorInfo, fg_ThisActor(m_pThis), m_BackupKey, m_bBackupFinishedStarting)
									, fg_Format("Backup for '{}'", _ActorInfo.m_HostInfo.f_GetDesc())
								)
							;
							if (m_bBackupFinishedStarting)
								f_BackupInstance_ReportFinishedStarting(BackupInstance);
						}
					)
				;
				m_BackupManagers.f_OnRemoveActor
					(
						[this](TCWeakDistributedActor<CActor> const &_Actor, CTrustedActorInfo &&_ActorInfo)
						{
							auto *pInstance = m_RunningBackupInstances.f_FindEqual(_Actor);
							if (!pInstance)
								return;

							if (m_pCanDestroyTracker)
								pInstance->m_Instance.f_Destroy() > m_pCanDestroyTracker->f_Track();
							else
								pInstance->m_Instance.f_Destroy() > fg_DiscardResult();
							
							m_RunningBackupInstances.f_Remove(_Actor);
						}
					)
				;
			}
		;
	}
}
