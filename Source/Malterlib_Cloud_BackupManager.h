// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Cryptography/Hashes/SHA>
#include <Mib/Encoding/EJSON>
#include <Mib/File/DirectoryManifest>
#include <Mib/File/DirectorySync>

#include "Malterlib_Cloud_FileTransfer.h"

namespace NMib::NCloud
{
	enum
	{
		EBackupManagerMinProtocolVersion = 0x101
		, EBackupManagerProtocolVersion = 0x102
	};
	
	struct CBackupManagerBackup : public NConcurrency::CActor
	{
		enum : uint32 
		{
			EMinProtocolVersion = EBackupManagerMinProtocolVersion
			, EProtocolVersion = EBackupManagerProtocolVersion
		};
		
		enum EManifestChange
		{
			EManifestChange_Change
			, EManifestChange_Add
			, EManifestChange_Remove
			, EManifestChange_Rename
		};
		
		struct CStartBackupResult
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			NContainer::TCMap<NStr::CStr, uint64> m_FilesNotUpToDate;
		};

		struct CManifestChange_Change
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NFile::CDirectoryManifestFile m_ManifestFile;
		};

		struct CManifestChange_Add : public CManifestChange_Change
		{
		};

		struct CManifestChange_Remove
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		};

		struct CManifestChange_Rename : public CManifestChange_Change 
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_FromFileName;
		};

		using CManifestChange = NContainer::TCStreamableVariant
			<
				EManifestChange
				, CManifestChange_Change, EManifestChange_Change
				, CManifestChange_Add, EManifestChange_Add
				, CManifestChange_Remove, EManifestChange_Remove
				, CManifestChange_Rename, EManifestChange_Rename
			>
		;		

		CBackupManagerBackup();

		virtual NConcurrency::TCContinuation<NConcurrency::TCActorSubscriptionWithID<>> f_StartManifestRSync
			(
				NConcurrency::TCActorFunctorWithID<NConcurrency::TCContinuation<NContainer::CSecureByteVector> (NContainer::CSecureByteVector &&_Packet)> &&_fRunProtocol
				, uint64 _ManifestSize
			) = 0
		;
		virtual NConcurrency::TCContinuation<CStartBackupResult> f_StartBackup() = 0;

		virtual NConcurrency::TCContinuation<void> f_ManifestChange(NStr::CStr const &_FileName, CManifestChange const &_Change) = 0;

		virtual NConcurrency::TCContinuation<NConcurrency::TCActorSubscriptionWithID<>> f_StartRSync
			(
				NStr::CStr const &_FileName
				, NConcurrency::TCActorFunctorWithID<NConcurrency::TCContinuation<NContainer::CSecureByteVector> (NContainer::CSecureByteVector &&_Packet)> &&_fRunProtocol
			) = 0
		;
		
		virtual NConcurrency::TCContinuation<void> f_UploadData(NStr::CStr const &_FileName, uint64 _Position, NContainer::CSecureByteVector &&_Data) = 0;

		virtual NConcurrency::TCContinuation<void> f_InitialBackupFinished() = 0;
	};
	
	struct CBackupManager : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/BackupManager";
		
		enum : uint32
		{
			EMinProtocolVersion = EBackupManagerMinProtocolVersion
			, EProtocolVersion = EBackupManagerProtocolVersion
		};
		
		CBackupManager();
		
		static bool fs_IsValidProtocolVersion(uint32 _Version);
	
		static bool fs_IsValidHostname(NStr::CStr const &_String);
		static bool fs_IsValidBackupSource(NStr::CStr const &_String, NStr::CStr *o_pFriendlyName, NStr::CStr *o_pHostID);
		static bool fs_IsValidBackup(NStr::CStr const &_String, NStr::CStr *o_pBackupID, NTime::CTime *o_pTime);
		
		struct CBackupKey
		{
			NStr::CStr m_FriendlyName;
			NTime::CTime m_Time;
			NStr::CStr m_ID;

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		};
		
		struct CBackupID
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			void f_Format(NStr::CStrAggregate &o_Str) const;
			
			NTime::CTime m_Time;
			NStr::CStr m_ID;
		};
		
		// Deprecated - Start
		struct CStartBackup
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NStr::CStr m_FriendlyName;
				uint64 m_BackupSize;
				uint64 m_OplogSize;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			CBackupKey m_BackupKey;
		};
		
		struct CUploadData
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				uint32 m_Version = 0;
			};
			
			enum EFlag
			{
				EFlag_None = 0
				, EFlag_Finished = DMibBit(0)
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CBackupKey m_BackupKey;
			NStr::CStr m_File;
			uint64 m_Position;
			uint64 m_Size;
			EFlag m_Flags = EFlag_None;
			NContainer::CSecureByteVector m_Data;
		};
		
		struct CStopBackup
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			CBackupKey m_BackupKey;
		};
		
		struct CStartDownloadBackup
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
				
				NConcurrency::CActorSubscription m_Subscription;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			NStr::CStr f_GetDesc() const;
			
			NStr::CStr m_BackupSource;
			CBackupID m_BackupID;
			CFileTransferContext m_TransferContext;
		};
		// Deprecated - End

		
		// Deprecated - Start
		virtual NConcurrency::TCContinuation<CStartBackup::CResult> f_StartBackup(CStartBackup &&_Params);
		virtual NConcurrency::TCContinuation<CStopBackup::CResult> f_StopBackup(CStopBackup &&_Params);
		virtual NConcurrency::TCContinuation<CUploadData::CResult> f_UploadData(CUploadData &&_Params);
		virtual NConcurrency::TCContinuation<CStartDownloadBackup::CResult> f_StartDownloadBackup(CStartDownloadBackup &&_Params);
		// Deprecated - End

		virtual auto f_InitBackup(CBackupKey const &_BackupKey, NConcurrency::TCActorSubscriptionWithID<> &&_Subscription)
			-> NConcurrency::TCContinuation<NConcurrency::TCDistributedActorInterfaceWithID<CBackupManagerBackup>> = 0
		;
		
		virtual NConcurrency::TCContinuation<NContainer::TCVector<NStr::CStr>> f_ListBackupSources() = 0;
		virtual NConcurrency::TCContinuation<NContainer::TCMap<NStr::CStr, NContainer::TCVector<CBackupID>>> f_ListBackups(NStr::CStr const &_ForBackupSource) = 0;
		
		virtual auto f_DownloadBackup(NStr::CStr const &_BackupSource, CBackupID const &_BackupID, NTime::CTime const &_Time, NConcurrency::TCActorSubscriptionWithID<> &&_Subscription)
			-> NConcurrency::TCContinuation<NConcurrency::TCDistributedActorInterfaceWithID<NFile::CDirectorySyncClient>>
			= 0
		;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_BackupManager.hpp"
