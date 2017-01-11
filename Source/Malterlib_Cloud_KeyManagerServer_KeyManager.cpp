// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/DistributedActor>

#include "Malterlib_Cloud_KeyManager.h"
#include "Malterlib_Cloud_KeyManagerServer.h"
#include "Malterlib_Cloud_KeyManagerServer_Internal.h"

namespace NMib::NCloud
{
	CKeyManager::CInternal::CInternal(CKeyManager *_pThis, NConcurrency::TCWeakActor<CKeyManagerServer> const &_ServerActor)
		: m_pThis(_pThis)
		, m_ServerActor(_ServerActor)
	{
	}
	
	CKeyManager::CKeyManager(NConcurrency::TCWeakActor<CKeyManagerServer> const &_ServerActor)
		: mp_pInternal(fg_Construct(this, _ServerActor))
	{
		DMibPublishActorFunction(CKeyManager::f_RequestKey);
	}
	
	CKeyManager::~CKeyManager()
	{
	}
	
	NConcurrency::TCContinuation<CSymmetricKey> CKeyManager::f_RequestKey(NStr::CStr const &_Identifier, uint32 _KeySize)
	{
		auto &Internal = *mp_pInternal;
		
		auto ServerActor = Internal.m_ServerActor.f_Lock();

		NConcurrency::TCContinuation<CSymmetricKey> Continuation;
		
		if (!ServerActor)
		{
			Continuation.f_SetException(DMibErrorInstance("Key manager server was deleted"));
			return Continuation;
		}
		
		ServerActor(&CKeyManagerServer::fp_RequestKey, NConcurrency::CActorDistributionManager::fs_GetCallingHostInfo().f_GetRealHostID(), _Identifier, _KeySize) > [Continuation](NConcurrency::TCAsyncResult<CSymmetricKey> &&_Result)
			{
				Continuation.f_SetResult(fg_Move(_Result));
			}
		;
		return Continuation;
	}
}
