// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_BackupManagerClient_Internal.h"
#include "Malterlib_Cloud_BackupManagerClient_BackupInstance.h"

namespace NMib::NCloud
{
	CBackupManagerClient::CInternal::CInternal
		(
			CBackupManagerClient *_pThis
			, CConfig const &_Config
			, TCActor<CDistributedActorTrustManager> const &_TrustManager
			, TCActorFunctor
			<
				TCFuture<TCActorSubscriptionWithID<>>
				(
					TCDistributedActorInterfaceWithID<CDistributedAppInterfaceBackup> _BackupInterface
					, CActorSubscription _ManifestFinished
					, CStr _BackupRoot
				)
			>
			&&_fOnNewBackup
		)
		: m_pThis(_pThis)
		, m_pDestroyed(fg_Construct(false))
		, m_Config(_Config)
		, m_TrustManager(_TrustManager)
		, m_fOnNewBackup(fg_Move(_fOnNewBackup))
	{
	}
	
	CBackupManagerClient::CInternal::~CInternal() = default;
}
