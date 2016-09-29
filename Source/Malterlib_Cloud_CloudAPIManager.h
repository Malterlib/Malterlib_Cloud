// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJSON>
#include <Mib/Concurrency/DistributedActor>

namespace NMib::NCloud
{
	struct CCloudAPIManager : public NConcurrency::CActor
	{
		using CDistributedActorWriteStream = NConcurrency::CDistributedActorWriteStream;
		using CDistributedActorReadStream = NConcurrency::CDistributedActorReadStream;

		enum 
		{
			EMinProtocolVersion = 0x101
			, EProtocolVersion = 0x102
		};

		struct CEnsureContainer
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
			};
			
			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_CloudContext;
			NStr::CStr m_ContainerName;
			NStr::CStr m_TempURLKey; /// If this is left empty the container is locked 
		};

		static bool fs_IsValidCloudContext(NStr::CStr const &_String);
		static bool fs_IsValidContainerName(NStr::CStr const &_String);
		static bool fs_IsValidTempURLKey(NStr::CStr const &_String);
		
		virtual NConcurrency::TCContinuation<CEnsureContainer::CResult> f_EnsureContainer(CEnsureContainer &&_Params) = 0;
	};
}
