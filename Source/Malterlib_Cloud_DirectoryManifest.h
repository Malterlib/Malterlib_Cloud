// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Cryptography/Hashes/SHA>

namespace NMib::NCloud
{
	enum EDirectoryManifestSyncFlag /// \brief Flag for how to sync specific files
	{
		EDirectoryManifestSyncFlag_None = 0							///< Normal syncing. In this case the rsync is used for syncing changes
		, EDirectoryManifestSyncFlag_Append = DMibBit(0)			///< Append syncing. Any changes are assumed to be append only
		, EDirectoryManifestSyncFlag_TransactionLog = DMibBit(1)	///< Should be used together with ESyncFlag_Append. This tells the backup manager to sync writes to disk as quickly as possible.
	};
	
	struct CDirectoryManifestFile
	{
		bool operator == (CDirectoryManifestFile const &_Right) const;
		
		NStr::CStr const &f_GetFileName() const;
		bool f_IsDirectory() const;
		bool f_IsFile() const;
		
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream, uint32 _Version);

		static EDirectoryManifestSyncFlag fs_ParseSyncFlags(NEncoding::CEJSON const &_JSON);
		static NEncoding::CEJSON fs_GenerateSyncFlags(EDirectoryManifestSyncFlag _Flags);

		NDataProcessing::CHashDigest_SHA256 m_Digest;
		uint64 m_Length = 0;
		NTime::CTime m_WriteTime;
		NStr::CStr m_SymlinkData;
		NFile::EFileAttrib m_Attributes = NFile::EFileAttrib_None;
		NStr::CStr m_Owner;
		NStr::CStr m_Group;
		EDirectoryManifestSyncFlag m_Flags = EDirectoryManifestSyncFlag_None;
	};

	struct CDirectoryManifestConfig
	{
		static NStr::CStr fs_ParseWildcard(NStr::CStr const &_Wildcard, bool &o_bRecursive);
		CDirectoryManifestConfig const &f_Validate() const;
		
		NStr::CStr m_Root;																		///< The root directory of the backup.
		NContainer::TCSet<NStr::CStr> m_IncludeWildcards = {"^*"};								///< \brief Relative to m_Root. This is a file search. Only file name can have wildcards.
																								/// Use ^ in the beginning of the file path to create a recursive search.
		NContainer::TCSet<NStr::CStr> m_ExcludeWildcards;										///< Relative to m_Root. Evaluated after include wild cards as a filtering step.
		NContainer::TCMap<NStr::CStr, EDirectoryManifestSyncFlag> m_AddSyncFlagsWildcards;		///< Relative to m_Root.
		NContainer::TCMap<NStr::CStr, EDirectoryManifestSyncFlag> m_RemoveSyncFlagsWildcards;	///< Relative to m_Root. Evaluated after m_AddSyncFlagsWildcards.
	};
	
	struct CDirectoryManifest
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);
		
		NEncoding::CEJSON f_ToJSON() const;
		static CDirectoryManifest fs_FromJSON(NEncoding::CEJSON const &_JSON);
		
		enum
		{
			EManifestStreamVersion = 0x101
		};
		
		NContainer::TCMap<NStr::CStr, CDirectoryManifestFile> m_Files;
		
		static void fs_UpdateManifestFile(CDirectoryManifestConfig const &_Config, NStr::CStr const &_FileName, CDirectoryManifestFile &o_ManifestFile);
		static CDirectoryManifest fs_GetManifest(CDirectoryManifestConfig const &_Config, NFunction::TCFunctionNoAlloc<void ()> const &_fCheckAbort);
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_DirectoryManifest.hpp"
