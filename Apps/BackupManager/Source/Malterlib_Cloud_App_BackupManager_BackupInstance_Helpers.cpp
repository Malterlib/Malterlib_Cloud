// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"
#include "Malterlib_Cloud_App_BackupManager_BackupInstance_Internal.h"

namespace NMib::NCloud::NBackupManager
{
	TCFuture<CActorSubscription> CBackupInstance::CInternal::f_SequenceMultipleSyncs(TCVector<CStr> _Files)
	{
		if (_Files.f_IsEmpty())
		{
			co_return g_ActorSubscription / []
				{
				}
			;
		}
		else if (_Files.f_GetLen() == 1)
			co_return co_await f_SequenceSyncs(_Files[0]);

		TCFutureVector<CActorSubscription> PendingSubscriptions;
		for (auto &File : _Files)
			f_SequenceSyncs(File) > PendingSubscriptions;

		auto Subscriptions = co_await fg_AllDone(PendingSubscriptions);

		co_return g_ActorSubscription / [Subscriptions = fg_Move(Subscriptions)]() -> TCFuture<void>
			{
				TCFutureVector<void> Destroys;

				for (auto &Subscription : Subscriptions)
					Subscription->f_Destroy() > Destroys;

				co_await fg_AllDone(Destroys);

				co_return {};
			}
		;
	}

	TCFuture<CActorSubscription> CBackupInstance::CInternal::f_SequenceSyncs(CStr _FileName)
	{
		co_return co_await m_SequencedSyncs[_FileName].m_Sequencer.f_Sequence();
	}

	COnScopeExitShared CBackupInstance::CInternal::f_FilePending(CStr const &_FileName)
	{
		++m_PendingSyncs[_FileName];

		return g_OnScopeExitActor / [this, _FileName]
			{
				if ((--m_PendingSyncs[_FileName]) == 0)
				{
					m_PendingSyncs.f_Remove(_FileName);
					fp_UpdatePendingQuiescence();
				}
			}
		;
	}

	void CBackupInstance::CInternal::fp_UpdatePendingQuiescence()
	{
		if (!m_PendingSyncs.f_IsEmpty())
			return;
		auto PendingSyncsQuiescence = fg_Move(m_PendingSyncsQuiescence);
		for (auto &fOnQuiescence : PendingSyncsQuiescence)
			fOnQuiescence();
	}

	void CBackupInstance::CInternal::f_OnPendingQuiescence(TCFunctionMutable<void ()> &&_fOnQuiescence)
	{
		if (m_PendingSyncs.f_IsEmpty())
		{
			_fOnQuiescence();
			return;
		}
		m_PendingSyncsQuiescence.f_Insert(fg_Move(_fOnQuiescence));
	}
}
