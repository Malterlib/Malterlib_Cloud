// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Cryptography/Hashes/SHA>
#include <Mib/Encoding/EJSON>

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
		
		enum EManifestSyncFlag /// \brief Flag for how to sync specific files
		{
			EManifestSyncFlag_None = 0						///< Normal syncing. In this case the rsync is used for syncing changes
			, EManifestSyncFlag_Append = DMibBit(0)			///< Append syncing. Any changes are assumed to be append only
			, EManifestSyncFlag_TransactionLog = DMibBit(1) ///< Should be used together with ESyncFlag_Append. This tells the backup manager to sync writes to disk as quickly as possible.
		};
		
		enum EManifestChange
		{
			EManifestChange_Change
			, EManifestChange_Add
			, EManifestChange_Remove
			, EManifestChange_Rename
		};
		
		struct CManifestFile
		{
			bool operator == (CManifestFile const &_Right) const;
			
			NStr::CStr const &f_GetFileName() const;
			bool f_IsDirectory() const;
			
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NDataProcessing::CHashDigest_SHA256 m_Digest;
			uint64 m_Length = 0;
			NTime::CTime m_WriteTime;
			NStr::CStr m_SymlinkData;
			NFile::EFileAttrib m_Attributes = NFile::EFileAttrib_None;
			NStr::CStr m_Owner;
			NStr::CStr m_Group;
			EManifestSyncFlag m_Flags = EManifestSyncFlag_None;
		};
		
		struct CManifest
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			NEncoding::CEJSON f_ToJSON() const;
			static CManifest fs_FromJSON(NEncoding::CEJSON const &_JSON);
			
			NContainer::TCMap<NStr::CStr, CManifestFile> m_Files;
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

			CManifestFile m_ManifestFile;
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
		
		static EManifestSyncFlag fs_ParseSyncFlags(NEncoding::CEJSON const &_JSON);
		static NEncoding::CEJSON fs_GenerateSyncFlags(EManifestSyncFlag _Flags);
		
		virtual NConcurrency::TCContinuation<CStartBackupResult> f_StartBackup(CManifest const &_Manifest) = 0;

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
		// Deprecated - End
		
		struct CListBackupSources
		{
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
				
				NContainer::TCVector<NStr::CStr> m_BackupSources;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		};

		struct CListBackups
		{
			struct CBackup
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
				
				void f_Format(NStr::CStrAggregate &o_Str) const;
				
				NTime::CTime m_Time;
				NStr::CStr m_BackupID;
				
			};
			
			struct CResult
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);
				
				void f_Format(NStr::CStrAggregate &o_Str) const;
				
				NContainer::TCMap<NStr::CStr, NContainer::TCVector<CBackup>> m_Backups;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_ForBackupSource;
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
			NTime::CTime m_Time;
			NStr::CStr m_BackupID;
			CFileTransferContext m_TransferContext;
		};
		
		// Deprecated - Start
		virtual NConcurrency::TCContinuation<CStartBackup::CResult> f_StartBackup(CStartBackup &&_Params);
		virtual NConcurrency::TCContinuation<CStopBackup::CResult> f_StopBackup(CStopBackup &&_Params);
		virtual NConcurrency::TCContinuation<CUploadData::CResult> f_UploadData(CUploadData &&_Params);
		// Deprecated - End

		virtual auto f_InitBackup(CBackupKey const &_BackupKey, NConcurrency::TCActorSubscriptionWithID<> &&_Subscription)
			-> NConcurrency::TCContinuation<NConcurrency::TCDistributedActorInterfaceWithID<CBackupManagerBackup>> = 0
		;
		
		virtual NConcurrency::TCContinuation<CListBackupSources::CResult> f_ListBackupSources(CListBackupSources &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CListBackups::CResult> f_ListBackups(CListBackups &&_Params) = 0;
		virtual NConcurrency::TCContinuation<CStartDownloadBackup::CResult> f_StartDownloadBackup(CStartDownloadBackup &&_Params) = 0;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif
