// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Encoding/EJSON>
#include <Mib/Concurrency/DistributedActor>

namespace NMib::NCloud
{
	DMibImpErrorClassDefine(CExceptionCloudAPI, NException::CException);
		
#	define DMibErrorCloudAPI(_Description) DMibImpError(NMib::NCloud::CExceptionCloudAPI, _Description)

#	ifndef DMibPNoShortCuts
#		define DErrorCloudAPI(_Description) DMibErrorCloudAPI(_Description)
#	endif
	
	struct CCloudAPIManager : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/CloudAPIManager";
		
		using CDistributedActorWriteStream = NConcurrency::CDistributedActorWriteStream;
		using CDistributedActorReadStream = NConcurrency::CDistributedActorReadStream;
		
		CCloudAPIManager();

		enum : uint32
		{
			EProtocolVersion_Min = 0x101
			, EProtocolVersion_Current = 0x102
		};
		
		struct CGetSwiftBaseURL
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NStr::CStr m_BaseURL;
			};
			
			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_CloudContext;
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
		
		struct CSignTempURL
		{
			struct CResult
			{
				void f_Feed(CDistributedActorWriteStream &_Stream) const;
				void f_Consume(CDistributedActorReadStream &_Stream);
				
				NStr::CStr m_SignedURL;
			};
			
			void f_Feed(CDistributedActorWriteStream &_Stream) const;
			void f_Consume(CDistributedActorReadStream &_Stream);
			
			NStr::CStr m_CloudContext;
			NStr::CStr m_Method;
			NStr::CStr m_ContainerName;
			NStr::CStr m_ObjectId;
			NStr::CStr m_TempURLKey;
		};
		
		struct CDeleteObject
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
			NStr::CStr m_ObjectId;
		};

		static bool fs_IsValidCloudContext(NStr::CStr const &_String);
		static bool fs_IsValidContainerName(NStr::CStr const &_String);
		static bool fs_IsValidTempURLKey(NStr::CStr const &_String);
		static bool fs_IsValidMethod(NStr::CStr const &_String);
		static bool fs_IsValidObjectId(NStr::CStr const &_String);
		
		virtual NConcurrency::TCFuture<CGetSwiftBaseURL::CResult> f_GetSwiftBaseURL(CGetSwiftBaseURL _Params) = 0;
		virtual NConcurrency::TCFuture<CEnsureContainer::CResult> f_EnsureContainer(CEnsureContainer _Params) = 0;
		virtual NConcurrency::TCFuture<CSignTempURL::CResult> f_SignTempURL(CSignTempURL _Params) = 0;
		virtual NConcurrency::TCFuture<CDeleteObject::CResult> f_DeleteObject(CDeleteObject _Params) = 0;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
