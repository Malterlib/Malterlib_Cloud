#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Cloud_KeyManager_Shared.h"

namespace NMib
{
	namespace NCloud
	{
		class CKeyManagerServerInternal;
		
		class CKeyManager : public NConcurrency::CActor
		{
		public:
			CKeyManager(NConcurrency::TCActor<CKeyManagerServerInternal> &_InternalActor);
			~CKeyManager();
			
			NConcurrency::TCContinuation<CSymmetricKey> f_RequestKey(NStr::CStr const &_Identifier);
		private:
			struct CInternal;
			
			NIndirection::TCIndirection<CInternal> mp_Internal;
		};
	}
}
