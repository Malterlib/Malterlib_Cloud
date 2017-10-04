// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>

namespace NMib::NCloud
{
	DMibImpErrorClass(CExceptionSecrets, NException::CException);
		
#	define DMibErrorSecrets(_Description) DMibImpError(NMib::NCloud::CExceptionSecrets, _Description)

#	ifndef DMibPNoShortCuts
#		define DErrorSecrets(_Description) DMibErrorSecrets(_Description)
#	endif
	
	struct CSecretsManager : public NConcurrency::CActor
	{
		CSecretsManager();

		enum : uint32
		{
			EMinProtocolVersion = 0x101
			, EProtocolVersion = 0x101
		};
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif
