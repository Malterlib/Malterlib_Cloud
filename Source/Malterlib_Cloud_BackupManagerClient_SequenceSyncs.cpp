// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/ActorSubscription>

#include "Malterlib_Cloud_BackupManagerClient_BackupInstance.h"

namespace NMib::NCloud::NPrivate
{
	void CBackupManagerClient_Instance::fp_RunSequencedSyncs(CStr const &_FileName)
	{
		auto pSequencedSync = mp_SequencedSyncs.f_FindEqual(_FileName);
		if (!pSequencedSync)
			return;

		if (pSequencedSync->m_WriteWaiting.f_IsEmpty() && pSequencedSync->m_ReadWaiting.f_IsEmpty() && pSequencedSync->m_nReading == 0 && pSequencedSync->m_nWriting == 0)
		{
			mp_SequencedSyncs.f_Remove(_FileName);
			return;
		}

		if (!pSequencedSync->m_WriteWaiting.f_IsEmpty())
		{
			if (pSequencedSync->m_nReading || pSequencedSync->m_nWriting)
				return;
			++pSequencedSync->m_nWriting;
			pSequencedSync->m_WriteWaiting.f_Pop()
				(
					g_OnScopeExitActor / [this, _FileName]
					{
						auto pSequencedSync = mp_SequencedSyncs.f_FindEqual(_FileName);
						if (pSequencedSync)
							--pSequencedSync->m_nWriting;
						fp_RunSequencedSyncs(_FileName);
					}
				)
			;
		}

		if (pSequencedSync->m_nWriting)
			return;

		while (!pSequencedSync->m_ReadWaiting.f_IsEmpty())
		{
			++pSequencedSync->m_nReading;
			pSequencedSync->m_ReadWaiting.f_Pop()
				(
					g_OnScopeExitActor / [this, _FileName]
					{
						auto pSequencedSync = mp_SequencedSyncs.f_FindEqual(_FileName);
						if (pSequencedSync)
							--pSequencedSync->m_nReading;
						fp_RunSequencedSyncs(_FileName);
					}
				)
			;
		}
	}

	namespace
	{
		struct CMultipleSyncsState
		{
			umint m_nWaiting = 0;
			TCVector<COnScopeExitShared> m_CleanupScopes;
			TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> m_fToRun;
		};
	}

	void CBackupManagerClient_Instance::fp_SequenceMultipleSyncs
		(
			TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> &&_fToRun
			, TCVector<CStr> const &_WriteFiles
			, TCVector<CStr> const &_ReadFiles
		)
	{
		if (_WriteFiles.f_IsEmpty() && _ReadFiles.f_IsEmpty())
		{
			_fToRun
				(
					g_OnScopeExitShared / []
					{
					}
				)
			;
			return;
		}
		else if (_WriteFiles.f_GetLen() == 1 && _ReadFiles.f_IsEmpty())
			return fp_SequenceWriteSyncs(_WriteFiles[0], fg_Move(_fToRun));
		else if (_ReadFiles.f_GetLen() == 1 && _WriteFiles.f_IsEmpty())
			return fp_SequenceReadSyncs(_ReadFiles[0], fg_Move(_fToRun));

		TCSharedPointer<CMultipleSyncsState> pState = fg_Construct();
		pState->m_nWaiting = _WriteFiles.f_GetLen() + _ReadFiles.f_GetLen();
		pState->m_fToRun = fg_Move(_fToRun);

		auto fSyncRun = [pState](COnScopeExitShared &&_pCleanup)
			{
				pState->m_CleanupScopes.f_Insert(fg_Move(_pCleanup));
				if (--pState->m_nWaiting == 0)
				{
					pState->m_fToRun
						(
							g_OnScopeExitShared / [CleanupScopes = fg_Move(pState->m_CleanupScopes)]
							{
							}
						)
					;
					pState->m_fToRun.f_Clear();
				}
			}
		;

		for (auto &File : _WriteFiles)
			fp_SequenceWriteSyncs(File, fSyncRun);

		for (auto &File : _ReadFiles)
			fp_SequenceReadSyncs(File, fSyncRun);
	}

	void CBackupManagerClient_Instance::fp_SequenceWriteSyncs(CStr const &_FileName, TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> &&_fToRun)
	{
		mp_SequencedSyncs[_FileName].m_WriteWaiting.f_Insert(fg_Move(_fToRun));
		fp_RunSequencedSyncs(_FileName);
	}

	void CBackupManagerClient_Instance::fp_SequenceReadSyncs(CStr const &_FileName, TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> &&_fToRun)
	{
		mp_SequencedSyncs[_FileName].m_ReadWaiting.f_Insert(fg_Move(_fToRun));
		fp_RunSequencedSyncs(_FileName);
	}
}
