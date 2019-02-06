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
		, EBackupManagerProtocolVersion = 0x104
	};

	DMibImpErrorClassDefine(CExceptionBackupManagerHashMismatch, NException::CException);

#	define DMibErrorBackupManagerHashMismatch(_Description) DMibImpError(NMib::NCloud::CExceptionBackupManagerHashMismatch, _Description)
#	define DMibErrorInstanceBackupManagerHashMismatch(_Description) DMibImpErrorInstance(NMib::NCloud::CExceptionBackupManagerHashMismatch, _Description)

//#	define DMibCloudBackupManagerDebug

#	if defined(DMibCloudBackupManagerDebug)
#		define DMibCloudBackupManagerDebugOut DMibConOut2
#	else
#		define DMibCloudBackupManagerDebugOut(...)  (void)0
#	endif

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

		enum EInitialBackupFinishedFlag
		{
			EInitialBackupFinishedFlag_None
			, EInitialBackupFinishedFlag_ReturnChanges = DMibBit(0)
		};

		struct CManifestFile : public NFile::CDirectoryManifestFile
		{
#ifdef DCompiler_MSVC_Workaround
			CManifestFile();
			CManifestFile(NFile::CDirectoryManifestFile const &_ManifestFile);
#endif

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NFile::CDirectoryManifestFile &f_ManifestFile();
		};

		struct CStartBackupResult
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			NContainer::TCMap<NStr::CStr, NCryptography::CHashDigest_SHA256> m_FilesNotUpToDate;
		};

		struct CManifestChange_Change
		{
#ifdef DCompiler_MSVC_Workaround
			CManifestChange_Change();
			CManifestChange_Change(CManifestFile const &_ManifestFile);
#endif

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CManifestFile m_ManifestFile;
		};

		struct CManifestChange_Add : public CManifestChange_Change
		{
			CManifestChange_Add() = default;
			CManifestChange_Add(CManifestChange_Add &&) = default;
			CManifestChange_Add(CManifestChange_Add const &) = default;

			CManifestChange_Add(NFile::CDirectoryManifestFile &&_ManifestFile);
		};

		struct CManifestChange_Remove
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		};

		struct CManifestChange_Rename : public CManifestChange_Change 
		{
			CManifestChange_Rename() = default;
			CManifestChange_Rename(CManifestChange_Rename &&) = default;
			CManifestChange_Rename(CManifestChange_Rename const &) = default;

			CManifestChange_Rename(NFile::CDirectoryManifestFile &&_ManifestFile, NStr::CStr const &_FromFileName);

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_FromFileName;
		};

		using CManifestChange = NStorage::TCStreamableVariant
			<
				EManifestChange
				, CManifestChange_Change, EManifestChange_Change
				, CManifestChange_Add, EManifestChange_Add
				, CManifestChange_Remove, EManifestChange_Remove
				, CManifestChange_Rename, EManifestChange_Rename
			>
		;

		struct CInitialBackupFinishedResult
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NContainer::TCVector<NStr::CStr> m_AddedFiles;
			NContainer::TCVector<NStr::CStr> m_RemovedFiles;
			NContainer::TCVector<NStr::CStr> m_UpdatedFiles;
		};

		struct CAppendData
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_FileName;
			uint64 m_Position;
			NContainer::CSecureByteVector m_Data;
			NCryptography::CHashDigest_SHA256 m_PreviousDigest;
			CManifestFile m_ManifestFile;
		};

		CBackupManagerBackup();

		static bool fs_ApplyManifestChange(NFile::CDirectoryManifest &o_Manifest, NStr::CStr const &_FileName, CManifestChange const &_Change);
		static bool fs_PretendApplyManifestChange(NFile::CDirectoryManifest const &_Manifest, NStr::CStr const &_FileName, CManifestChange const &_Change);
		static NFile::CDirectoryManifestFile const &fs_ManifestFileFromChange(CManifestChange const &_Change);
		static bool fs_ManifestChangeValid(NStr::CStr const &_FileName, CManifestChange const &_Change, NStr::CStr &o_Error);
		static bool fs_ManifestFileValid(NStr::CStr const &_FileName, NFile::CDirectoryManifestFile const &_File, NStr::CStr &o_Error);

		using FRunRSyncProtocol = NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<NContainer::CSecureByteVector> (NContainer::CSecureByteVector &&_Packet)>;

		virtual NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> f_StartManifestRSync
			(
			 	FRunRSyncProtocol &&_fRunProtocol
			 	, uint64 _ManifestSize
			 	, NCryptography::CHashDigest_SHA256 const &_ExpectedDigest
			) = 0
		;
		virtual NConcurrency::TCFuture<CStartBackupResult> f_StartBackup() = 0;

		virtual NConcurrency::TCFuture<void> f_ManifestChange(NStr::CStr const &_FileName, CManifestChange const &_Change) = 0;

		virtual NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> f_StartRSync
			(
			 	NStr::CStr const &_FileName
			 	, CManifestFile const &_ManifestFile
			 	, FRunRSyncProtocol &&_fRunProtocol
			) = 0
		;
		virtual NConcurrency::TCFuture<void> f_AppendData(NStr::CStr const &_FileName, CAppendData &&_Data) = 0;

		virtual NConcurrency::TCFuture<CInitialBackupFinishedResult> f_InitialBackupFinished(EInitialBackupFinishedFlag _FinishedFlags) = 0;
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
		static bool fs_IsValidBackupTime(NStr::CStr const &_String, NTime::CTime *o_pTime);

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
			bool operator < (CBackupID const &_Right) const;
			
			NTime::CTime m_Time;
			NStr::CStr m_ID;
		};
		
		enum EInitBackupFlag
		{
			EInitBackupFlag_None = 0
			, EInitBackupFlag_ForceNew = DMibBit(0)
		};

		struct CInitBackup
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CBackupKey m_BackupKey;
			NConcurrency::TCActorSubscriptionWithID<> m_Subscription;
			EInitBackupFlag m_Flags = EInitBackupFlag_None;
		};

		struct CDownloadBackup
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr f_GetDesc() const;

			NStr::CStr m_BackupSource;
			NTime::CTime m_Time;
			NConcurrency::TCActorSubscriptionWithID<> m_Subscription;
		};

		struct CBackupInfo
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			void f_Format(NStr::CStrAggregate &o_Str) const;

			NTime::CTime m_Earliest;
			NTime::CTime m_Latest;
			NContainer::TCVector<NTime::CTime> m_Snapshots;
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
		virtual NConcurrency::TCFuture<CStartBackup::CResult> f_StartBackup(CStartBackup &&_Params);
		virtual NConcurrency::TCFuture<CStopBackup::CResult> f_StopBackup(CStopBackup &&_Params);
		virtual NConcurrency::TCFuture<CUploadData::CResult> f_UploadData(CUploadData &&_Params);
		virtual NConcurrency::TCFuture<CStartDownloadBackup::CResult> f_StartDownloadBackup(CStartDownloadBackup &&_Params);
		// Deprecated - End

		virtual auto f_InitBackup(CInitBackup &&_Params)
			-> NConcurrency::TCFuture<NConcurrency::TCDistributedActorInterfaceWithID<CBackupManagerBackup>> = 0
		;
		
		virtual NConcurrency::TCFuture<NContainer::TCVector<NStr::CStr>> f_ListBackupSources() = 0;
		virtual NConcurrency::TCFuture<NContainer::TCMap<NStr::CStr, CBackupInfo>> f_ListBackups(NStr::CStr const &_ForBackupSource) = 0;
		
		virtual auto f_DownloadBackup(CDownloadBackup &&_DownloadBackup)
			-> NConcurrency::TCFuture<NConcurrency::TCDistributedActorInterfaceWithID<NFile::CDirectorySyncClient>>
			= 0
		;
	};
}

DMibConcurrencyRegisterMemberFunctionLowestVersion(NMib::NCloud::CBackupManager::f_InitBackup, 0x104);
DMibConcurrencyRegisterMemberFunctionLowestVersion(NMib::NCloud::CBackupManager::f_DownloadBackup, 0x103);
DMibConcurrencyRegisterMemberFunctionLowestVersion(NMib::NCloud::CBackupManager::f_ListBackups, 0x103);

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_BackupManager.hpp"
