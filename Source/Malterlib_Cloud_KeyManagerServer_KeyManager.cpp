
#include <Mib/Core/Core>
#include <Mib/Concurrency/DistributedActor>

#include "Malterlib_Cloud_KeyManager.h"
#include "Malterlib_Cloud_KeyManagerServer.h"
#include "Malterlib_Cloud_KeyManagerServer_Internal.h"

namespace NMib
{
	namespace NCloud
	{		
		CKeyManager::CInternal::CInternal(CKeyManager *_pThis, NConcurrency::TCActor<CKeyManagerServer> const &_ServerActor)
			: m_pThis(_pThis)
			, m_ServerActor(_ServerActor)
		{
			
		}
		
		CKeyManager::CKeyManager(NConcurrency::TCActor<CKeyManagerServer> const &_ServerActor)
			: mp_pInternal(fg_Construct(this, _ServerActor))
		{
		}
		
		CKeyManager::~CKeyManager()
		{
		}
		
		NConcurrency::TCContinuation<CSymmetricKey> CKeyManager::f_RequestKey(NStr::CStr const &_Identifier, uint32 _KeySize)
		{
			auto &Internal = *mp_pInternal;
			
			NConcurrency::TCContinuation<CSymmetricKey> Continuation;
			Internal.m_ServerActor(&CKeyManagerServer::fp_RequestKey, NConcurrency::CActorDistributionManager::fs_GetCallingHostID(), _Identifier, _KeySize) > [Continuation](NConcurrency::TCAsyncResult<CSymmetricKey> &&_Result)
				{
					Continuation.f_SetResult(fg_Move(_Result));
				}
			;
			return Continuation;
		}

	}
}
