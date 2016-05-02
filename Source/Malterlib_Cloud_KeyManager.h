// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Cloud_KeyManager_Shared.h"

namespace NMib
{
	namespace NCloud
	{
		class CKeyManagerServer;
		
		class CKeyManager : public NConcurrency::CActor
		{
			friend class CKeyManagerServer;
			
		public:
			CKeyManager(NConcurrency::TCWeakActor<CKeyManagerServer> const &_ServerActor);
			~CKeyManager();
			
			NConcurrency::TCContinuation<CSymmetricKey> f_RequestKey(NStr::CStr const &_Identifier, uint32 _KeySize);
		private:
			struct CInternal;
			
			NPtr::TCUniquePointer<CInternal> mp_pInternal;
		};
	}
}
