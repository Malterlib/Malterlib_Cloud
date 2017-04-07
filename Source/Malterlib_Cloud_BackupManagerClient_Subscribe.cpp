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
							if (m_pThis->mp_bDestroyed)
								return;
							m_RunningBackupInstances[_Actor] = fg_Construct
								(
									fg_Construct(_Actor, m_Manifest, m_Config, _ActorInfo, fg_ThisActor(m_pThis), m_BackupKey)
									, fg_Format("Backup for '{}'", _ActorInfo.m_HostInfo.f_GetDesc())
								)
							;
						}
					)
				;
				m_BackupManagers.f_OnRemoveActor
					(
						[this](TCWeakDistributedActor<CActor> const &_Actor)
						{
							auto *pActor = m_RunningBackupInstances.f_FindEqual(_Actor);
							if (!pActor)
								return;

							(*pActor)->f_Destroy() > m_pCanDestroyTracker->f_Track();
							
							m_RunningBackupInstances.f_Remove(_Actor);
						}
					)
				;
			}
		;
	}
}
