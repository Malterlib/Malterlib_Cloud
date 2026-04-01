// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/UUID>

#include "Malterlib_Cloud_App_DebugManager.h"

namespace NMib::NCloud::NDebugManager
{
	template <typename tf_CTo, typename tf_CType>
	tf_CTo fg_ConvertToDebugDatabase(tf_CType &&_Value)
	{
		if constexpr (NTraits::cIsSame<tf_CTo, CDebugDatabase::CAssetAdd>)
		{
			return tf_CTo
				{
					.m_Type = fg_ConvertToDebugDatabase<CDebugDatabase::EAssetType>(_Value.m_AssetType)
					, .m_Metadata = fg_ConvertToDebugDatabase<CDebugDatabase::CMetadata>(fg_TempCopy(_Value.m_Metadata))
					, .m_bForceOverwrite = fg_IsSet(_Value.m_Flags, CDebugManager::EUploadFlag::mc_ForceOverwrite)
				}
			;
		}
		else if constexpr (NTraits::cIsSame<tf_CTo, CDebugDatabase::CCrashDumpAdd>)
		{
			return tf_CTo
				{
					.m_ID = _Value.m_ID
					, .m_Metadata = fg_ConvertToDebugDatabase<CDebugDatabase::CMetadata>(fg_TempCopy(_Value.m_Metadata))
					, .m_ExceptionInfo = _Value.m_ExceptionInfo
					, .m_bForceOverwrite = fg_IsSet(_Value.m_Flags, CDebugManager::EUploadFlag::mc_ForceOverwrite)
				}
			;
		}
		else if constexpr (NTraits::cIsSame<tf_CTo, CDebugDatabase::CMetadata> || NTraits::cIsSame<tf_CTo, CDebugManager::CMetadata>)
		{
			return tf_CTo
				{
					.m_Product = fg_Move(_Value.m_Product)
					, .m_Application = fg_Move(_Value.m_Application)
					, .m_Configuration = fg_Move(_Value.m_Configuration)
					, .m_GitBranch = fg_Move(_Value.m_GitBranch)
					, .m_GitCommit = fg_Move(_Value.m_GitCommit)
					, .m_Platform = fg_Move(_Value.m_Platform)
					, .m_Version = fg_Move(_Value.m_Version)
					, .m_Tags = fg_Move(_Value.m_Tags)
				}
			;
		}
		else if constexpr (NTraits::cIsSame<tf_CTo, CDebugDatabase::EAssetType>)
		{
			switch (_Value)
			{
			case CDebugManager::EAssetType::mc_Executable: return CDebugDatabase::EAssetType::mc_Executable;
			case CDebugManager::EAssetType::mc_DebugInfo: return CDebugDatabase::EAssetType::mc_DebugInfo;
			}

			return CDebugDatabase::EAssetType::mc_Executable;
		}
		else if constexpr (NTraits::cIsSame<tf_CTo, CDebugManager::EAssetType>)
		{
			switch (_Value)
			{
			case CDebugDatabase::EAssetType::mc_Executable: return CDebugManager::EAssetType::mc_Executable;
			case CDebugDatabase::EAssetType::mc_DebugInfo: return CDebugManager::EAssetType::mc_DebugInfo;
			}

			return CDebugManager::EAssetType::mc_Executable;
		}
		else if constexpr (NTraits::cIsSame<tf_CTo, CDebugManager::CFileInfo>)
		{
			return tf_CTo
				{
					.m_FileName = fg_Move(_Value.m_FileName)
					, .m_Timestamp = _Value.m_Timestamp
					, .m_Digest = _Value.m_Digest
					, .m_Size = _Value.m_Size
					, .m_CompressedSize = _Value.m_CompressedSize
				}
			;
		}
		else if constexpr (NTraits::cIsSame<tf_CTo, CDebugManager::CAssetList::CAsset>)
		{
			return tf_CTo
				{
					.m_AssetType = fg_ConvertToDebugDatabase<CDebugManager::EAssetType>(_Value.m_AssetType)
					, .m_BuildID = fg_Move(_Value.m_BuildID)
					, .m_FileInfo = fg_ConvertToDebugDatabase<CDebugManager::CFileInfo>(fg_Move(_Value.m_FileInfo))
					, .m_Metadata = fg_ConvertToDebugDatabase<CDebugManager::CMetadata>(fg_Move(_Value.m_Metadata))
					, .m_MainAssetFile = fg_Move(_Value.m_MainAssetFile)
				}
			;
		}
		else if constexpr (NTraits::cIsSame<tf_CTo, CDebugManager::CCrashDumpList::CCrashDump>)
		{
			return tf_CTo
				{
					.m_ID = fg_Move(_Value.m_ID)
					, .m_FileInfo = fg_ConvertToDebugDatabase<CDebugManager::CFileInfo>(fg_Move(_Value.m_FileInfo))
					, .m_Metadata = fg_ConvertToDebugDatabase<CDebugManager::CMetadata>(fg_Move(_Value.m_Metadata))
					, .m_ExceptionInfo = fg_Move(_Value.m_ExceptionInfo)
				}
			;
		}
		else if constexpr (NTraits::cIsSame<tf_CTo, CDebugDatabase::CAssetFilter>)
		{
			return tf_CTo
				{
					.m_AssetType = _Value.m_AssetType
					? NStorage::TCOptional<CDebugDatabase::EAssetType>(fg_ConvertToDebugDatabase<CDebugDatabase::EAssetType>(*_Value.m_AssetType))
					: NStorage::TCOptional<CDebugDatabase::EAssetType>()
					, .m_BuildID = fg_Move(_Value.m_BuildID)
					, .m_FileName = fg_Move(_Value.m_FileName)
					, .m_TimestampStart = _Value.m_TimestampStart
					, .m_TimestampEnd = _Value.m_TimestampEnd
					, .m_Metadata = fg_ConvertToDebugDatabase<CDebugDatabase::CMetadata>(fg_Move(_Value.m_Metadata))
				}
			;
		}
		else if constexpr (NTraits::cIsSame<tf_CTo, CDebugDatabase::CCrashDumpFilter>)
		{
			return tf_CTo
				{
					.m_ID = fg_Move(_Value.m_ID)
					, .m_FileName = fg_Move(_Value.m_FileName)
					, .m_TimestampStart = _Value.m_TimestampStart
					, .m_TimestampEnd = _Value.m_TimestampEnd
					, .m_Metadata = fg_ConvertToDebugDatabase<CDebugDatabase::CMetadata>(fg_Move(_Value.m_Metadata))
					, .m_ExceptionInfo = fg_Move(_Value.m_ExceptionInfo)
				}
			;
		}
		else
			static_assert(false);
	}
}
