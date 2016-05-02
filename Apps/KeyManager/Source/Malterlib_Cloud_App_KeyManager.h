// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Daemon/Daemon>

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			aint fg_ProvidePassword();
			aint fg_GenerateTrustTicket();
			NPtr::TCUniquePointer<NService::CServiceImp> fg_CreateDaemon();
			
			struct ICCommandLine : public CActor
			{
				virtual TCContinuation<void> f_ProvidePassword(NStr::CStrSecure const &_Password) pure;
				virtual TCContinuation<CStr> f_GenerateTrustTicket() pure; 
			};
		}		
	}
}
