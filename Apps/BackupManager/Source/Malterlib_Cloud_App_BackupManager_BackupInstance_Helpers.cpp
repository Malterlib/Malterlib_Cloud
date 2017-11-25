
#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"
#include "Malterlib_Cloud_App_BackupManager_BackupInstance_Internal.h"

namespace NMib::NCloud::NBackupManager
{
	void CBackupInstance::CInternal::fp_RunSequencedSyncs(CStr const &_FileName)
	{
		auto pSequencedSync = m_SequencedSyncs.f_FindEqual(_FileName);
		if (!pSequencedSync)
			return;

		if (pSequencedSync->m_Waiting.f_IsEmpty())
		{
			m_SequencedSyncs.f_Remove(_FileName);
			return;
		}

		pSequencedSync->m_Waiting.f_Pop()
			(
				g_OnScopeExitActor > [this, _FileName]
			 	{
					fp_RunSequencedSyncs(_FileName);
				}
			)
		;
	}

	namespace
	{
		struct CMultipleSyncsState
		{
			mint m_nWaiting = 0;
			TCVector<COnScopeExitShared> m_CleanupScopes;
			TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> m_fToRun;
		};
	}

	void CBackupInstance::CInternal::f_SequenceMultipleSyncs(TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> &&_fToRun, TCVector<CStr> const &_Files)
	{
		if (_Files.f_IsEmpty())
		{
			_fToRun
				(
					g_OnScopeExitShared > []
					{
					}
				)
			;
			return;
		}
		else if (_Files.f_GetLen() == 1)
			return f_SequenceSyncs(_Files[0], fg_Move(_fToRun));

		TCSharedPointer<CMultipleSyncsState> pState = fg_Construct();
		pState->m_nWaiting = _Files.f_GetLen();
		pState->m_fToRun = fg_Move(_fToRun);

		for (auto &File : _Files)
		{
			f_SequenceSyncs
				(
				 	File
					, [pState](COnScopeExitShared &&_pCleanup)
				 	{
						pState->m_CleanupScopes.f_Insert(fg_Move(_pCleanup));
						if (--pState->m_nWaiting == 0)
						{
							pState->m_fToRun
								(
									g_OnScopeExitShared > [CleanupScopes = fg_Move(pState->m_CleanupScopes)]
								 	{
									}
								)
							;
							pState->m_fToRun.f_Clear();
						}
					}
			 	)
			;
		}
	}

	void CBackupInstance::CInternal::f_SequenceSyncs(CStr const &_FileName, TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> &&_fToRun)
	{
		if (auto pSequencedSync = m_SequencedSyncs.f_FindEqual(_FileName))
		{
			pSequencedSync->m_Waiting.f_Insert(fg_Move(_fToRun));
			return;
		}

		m_SequencedSyncs[_FileName].m_Waiting.f_Insert(fg_Move(_fToRun));

		fp_RunSequencedSyncs(_FileName);
	}

	COnScopeExitShared CBackupInstance::CInternal::f_FilePending(CStr const &_FileName)
	{
		++m_PendingSyncs[_FileName];

		return g_OnScopeExitActor > [this, _FileName]
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
