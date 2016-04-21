#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>

namespace NMib
{
	namespace NCloud
	{
		class CKeyManagerServerInternal : public CKeyManagerServer
		{
		};
		
		class CKeyManagerInternalInterface : public NConcurrency::CActor
		{
			// Synchronize databases
		};
		
		struct CKeyManagerServer::CInternal
		{
		};
		
		struct CKeyManager::CInternal
		{
		};
	}
}
