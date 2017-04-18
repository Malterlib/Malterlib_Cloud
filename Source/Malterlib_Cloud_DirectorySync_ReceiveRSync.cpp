// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_DirectorySync.h"
#include "Malterlib_Cloud_DirectorySync_ReceiveInternal.h"

namespace NMib::NCloud
{
	void CDirectorySyncReceive::CInternal::fsp_RunRSyncProtocol
		(
			TCSharedPointerSupportWeak<CRunningSyncState> const &_pState
			, CSecureByteVector &&_ServerPacket
			, TCContinuation<CByteStats> const &_Continuation
		)
	{
		auto &State = *_pState;
		if (!State.m_pClient)
		{
			if (!_Continuation.f_IsSet())
				_Continuation.f_SetException(DMibErrorInstance("RSync aborted"));
			return;
		}
		
		State.m_ByteStats.m_nIncoming += _ServerPacket.f_GetLen();
		
		bool bWantOneMoreProcess = true;
		bool bDone = false;
		while (bWantOneMoreProcess)
		{
			CSecureByteVector ToSendToServer;
			try
			{
				if (State.m_pClient->f_ProcessPacket(_ServerPacket, ToSendToServer, bWantOneMoreProcess))
					bDone = true;
			}
			catch (NException::CException const &_Exception)
			{
				if (!_Continuation.f_IsSet())
					_Continuation.f_SetException(DMibErrorInstance(fg_Format("Exceptio running RSync protocol: {}", _Exception.f_GetErrorStr())));
				return;
			}
			
			_ServerPacket.f_Clear();
			if (!ToSendToServer.f_IsEmpty())
			{
				State.m_ByteStats.m_nOutgoing += ToSendToServer.f_GetLen();

				State.m_fRunProtocol(fg_Move(ToSendToServer)) > [_pState, _Continuation](TCAsyncResult<CSecureByteVector> &&_ServerPacket)
					{
						if (!_ServerPacket)
						{
							if (!_Continuation.f_IsSet())
								_Continuation.f_SetException(DMibErrorInstance(fg_Format("Failed run RSync protocol: {}", _ServerPacket.f_GetExceptionStr())));
							return;
						}
						
						fsp_RunRSyncProtocol(_pState, fg_Move(*_ServerPacket), _Continuation);
					}
				;
			}
		}
		
		if (bDone)
		{
			State.m_bFinished = true;
			if (!_Continuation.f_IsSet())
				_Continuation.f_SetResult(State.m_ByteStats);
		}
	}
	
	TCContinuation<void> CDirectorySyncReceive::CInternal::f_RunRSyncProtocol(TCSharedPointerSupportWeak<CRunningSyncState> const &_pState)
	{
		TCContinuation<void> Continuation;
		g_Dispatch(m_FileActor) > [_pState]
			{
				TCContinuation<CByteStats> RunContinuation;
				fsp_RunRSyncProtocol(_pState, {}, RunContinuation);
				return RunContinuation;
			}
			> Continuation / [this, Continuation](CByteStats &&_Stats)
			{
				++m_Stats.m_nSyncedFiles;
				m_Stats.m_OutgoingBytes += _Stats.m_nOutgoing;
				m_Stats.m_IncomingBytes += _Stats.m_nIncoming;
				Continuation.f_SetResult();
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CDirectorySyncReceive::CInternal::f_RSync
		(
			TCFunctionMutable<bool (CRunningSyncState &_State)> &&_fInitRSync
			, TCFunctionMutable<TCContinuation<void> ()> &&_fOnDone
			, TCFunctionMutable<TCContinuation<CDirectorySyncClient::FRunRSync> (CActorSubscription &&_Subscription)> &&_fStartRSync
		)
	{
		CStr RSyncID = fg_RandomID();
		auto &pRSyncState = m_RSyncStates[RSyncID] = fg_Construct();
		pRSyncState->m_FileActor = m_FileActor;
		
		auto pCleanup = g_OnScopeExitActor > [this, RSyncID, pRSyncState]
			{
				m_RSyncStates.f_Remove(RSyncID);
				pRSyncState->f_Destroy() > fg_DiscardResult();
			}
		;

		TCContinuation<void> Continuation;
		g_Dispatch(m_FileActor) > [=, fInitRSync = fg_Move(_fInitRSync)]() mutable
			{
				return fInitRSync(*pRSyncState);
			}
			> Continuation / [=, fOnDone = fg_Move(_fOnDone), fStartRSync = fg_Move(_fStartRSync)](bool _bAlreadySynced) mutable
			{
				if (_bAlreadySynced)
				{
					fOnDone() > Continuation / [=]()
						{
							pRSyncState->f_Destroy() > Continuation / [=, pCleanup = pCleanup]()
								{
									Continuation.f_SetResult();
								}
							;
						}
					;
					return;
				}
				fStartRSync
					(
						g_ActorSubscription > [pRSyncState, Continuation]
						{
							if (!Continuation.f_IsSet() && !pRSyncState->m_bFinished)
								Continuation.f_SetException(DMibErrorInstance("Manifest rsync aborted prematurely"));
						}
					)
					> Continuation / [=, fOnDone = fg_Move(fOnDone)](CDirectorySyncClient::FRunRSync &&_fRunRSync) mutable
					{
						auto &RSyncState = *pRSyncState;
						RSyncState.m_fRunProtocol = fg_Move(_fRunRSync);
						f_RunRSyncProtocol(pRSyncState) > Continuation / [=, fOnDone = fg_Move(fOnDone)]() mutable
							{
								fOnDone() > Continuation / [=]()
									{
										pRSyncState->f_Destroy() > Continuation / [=, pCleanup = pCleanup]()
											{
												Continuation.f_SetResult();
											}
										;
									}
								;
							}
						;
					}
				;
			}
		;
		return Continuation;
	}
}
