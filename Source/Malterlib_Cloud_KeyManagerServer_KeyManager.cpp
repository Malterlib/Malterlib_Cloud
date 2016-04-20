
#include <Mib/Core/Core>

#include "Malterlib_Cloud_KeyManager.h"
#include "Malterlib_Cloud_KeyManagerServer.h"
#include "Malterlib_Cloud_KeyManagerServer_Internal.h"

namespace NMib
{
	namespace NCloud
	{
		CKeyManager::CKeyManager(NConcurrency::TCActor<CKeyManagerServerInternal> &_InternalActor)
		{
		}
		
		CKeyManager::~CKeyManager()
		{
		}
		
		NConcurrency::TCContinuation<CSymmetricKey> CKeyManager::f_RequestKey(NStr::CStr const &_Identifier)
		{
			return NConcurrency::TCContinuation<CSymmetricKey>::fs_Finished(CSymmetricKey());
		}

	}
}
