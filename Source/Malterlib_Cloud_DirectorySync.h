// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Storage/Variant>

#include "Malterlib_Cloud_DirectoryManifest.h"

namespace NMib::NCloud
{
	struct CDirectorySyncClient : public NConcurrency::CActor
	{
		enum : uint32 
		{
			EMinProtocolVersion = 0x101
			, EProtocolVersion = 0x101
		};

		using FRunRSync = NConcurrency::TCActorFunctorWithID<NConcurrency::TCContinuation<NContainer::CSecureByteVector> (NContainer::CSecureByteVector &&_Packet)>;
		
		CDirectorySyncClient();
		
		virtual NConcurrency::TCContinuation<FRunRSync> f_StartManifestRSync(NConcurrency::TCActorSubscriptionWithID<> &&_Subscription) = 0;
		virtual NConcurrency::TCContinuation<FRunRSync> f_StartRSync(NStr::CStr const &_FileName, NConcurrency::TCActorSubscriptionWithID<> &&_Subscription) = 0;
		virtual NConcurrency::TCContinuation<void> f_Finished() = 0;
	};
	
	struct CDirectorySyncStats
	{
		fp64 f_IncomingBytesPerSecond() const;
		fp64 f_OutgoingBytesPerSecond() const;

		uint64 m_nSyncedFiles = 0;
		uint64 m_OutgoingBytes = 0;
		uint64 m_IncomingBytes = 0;
		fp64 m_nSeconds = 0.0;
	};
	
	struct CDirectorySyncSend : public CDirectorySyncClient
	{
		struct CConfig
		{
			NStr::CStr m_BasePath;	///< No checks are done that files not specified in the manifest is downloaded from this directory.
			NContainer::TCVariant<CDirectoryManifest, CDirectoryManifestConfig, NStr::CStr> m_Manifest;	///< Specify manifest, a config to generate a manifest, or a path to a file
																										///< with an existing manifest that corresponds to the files available in m_BasePath.
		};

		struct CSyncResult
		{
			CDirectorySyncStats m_Stats;
			bool m_bFinished = false;
		};
		
		CDirectorySyncSend(CConfig &&_Config);
		~CDirectorySyncSend();
		
		NConcurrency::TCContinuation<CSyncResult> f_GetResult();

	private:
		struct CInternal;

		NConcurrency::TCContinuation<FRunRSync> f_StartManifestRSync(NConcurrency::TCActorSubscriptionWithID<> &&_Subscription) override;
		NConcurrency::TCContinuation<FRunRSync> f_StartRSync(NStr::CStr const &_FileName, NConcurrency::TCActorSubscriptionWithID<> &&_Subscription) override;
		NConcurrency::TCContinuation<void> f_Finished() override;

		NConcurrency::TCContinuation<void> fp_Destroy() override;
		
		NPtr::TCUniquePointer<CInternal> mp_pInternal;
	};

	struct CDirectorySyncReceive : public NConcurrency::CActor
	{
		enum EExcessFilesAction : uint32
		{
			EExcessFilesAction_Ignore = 0
			, EExcessFilesAction_Fail
			, EExcessFilesAction_Delete
		};

		enum ESyncFlag : uint32
		{
			ESyncFlag_None = 0
			, ESyncFlag_WriteTime = DMibBit(0)
			, ESyncFlag_Owner = DMibBit(1)
			, ESyncFlag_Group = DMibBit(2)
			, ESyncFlag_Attributes = DMibBit(3)
		};
		
		struct CConfig
		{
			NContainer::TCVariant<void, CDirectoryManifest, NStr::CStr> m_PreviousManifest;
			NStr::CStr m_OutputManifestPath;

			NStr::CStr m_TempDirectory = NFile::CFile::fs_GetTemporaryDirectory(); ///< Used to create temporary files used during RSync
			
			NStr::CStr m_BasePath;				///< The base path for destination files
			NStr::CStr m_PreviousBasePath;		///< The location to use for fallback source for rsync comparison
			
			NContainer::TCSet<NStr::CStr> m_IncludeWildcards; ///< Relative to m_BasePath. Leave empty to include all files in manifest
			NContainer::TCSet<NStr::CStr> m_ExcludeWildcards; ///< Relative to m_BasePath. Evaluated after include wild cards as a filtering step.
			
			EExcessFilesAction m_ExcessFilesAction = EExcessFilesAction_Ignore;
			ESyncFlag m_SyncFlags = ESyncFlag_WriteTime | ESyncFlag_Owner | ESyncFlag_Group | ESyncFlag_Attributes;
			
			uint32 m_QueueSize = 8*1024*1024;
			uint32 m_RSyncConcurrency = 8; ///< Number of simultaneous rsyncs to run.
		};
		
		struct CSyncResult
		{
			CDirectoryManifest m_Manifest;
			CDirectorySyncStats m_Stats;
		};
		
		CDirectorySyncReceive(CConfig &&_Config, NConcurrency::TCDistributedActorInterface<CDirectorySyncClient> &&_Client);
		~CDirectorySyncReceive();
		
		NConcurrency::TCContinuation<CSyncResult> f_PerformSync();
		
	private:
		struct CInternal;

		NConcurrency::TCContinuation<void> fp_Destroy() override;
		
		NPtr::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif
