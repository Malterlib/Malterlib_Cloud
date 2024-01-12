// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Cloud_KeyManager_Shared.h"

namespace NMib::NCloud
{
	enum : uint32
	{
		EKeyManagerProtocolVersion_Min = 0x101

		, EKeyManagerProtocolVersion_Current = 0x101
	};

	struct CKeyManager : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/KeyManager";
		
		enum : uint32
		{
			EProtocolVersion_Min = EKeyManagerProtocolVersion_Min
			, EProtocolVersion_Current = EKeyManagerProtocolVersion_Current
		};

		CKeyManager();
		~CKeyManager();
		
		virtual NConcurrency::TCFuture<CSymmetricKey> f_RequestKey(NStr::CStr const &_Identifier, uint32 _KeySize) = 0;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
