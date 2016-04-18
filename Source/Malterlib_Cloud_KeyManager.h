#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Cloud_KeyManager_Shared.h"

namespace NMib
{
	namespace NCloud
	{
		class CKeyManager : public NConcurrency::CActor
		{
			NConcurrency::TCContinuation<CSymmetricKey> f_RequestKey(NStr::CStr const &_Identifier);
		};
	}
}
