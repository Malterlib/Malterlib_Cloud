// Copyright © 2025 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void CDebugManager::CDownloadFileContents::f_Stream(tf_CStream &_Stream)
	{
		_Stream % fg_Move(m_DataGenerator);
		_Stream % fg_Move(m_Subscription);
		_Stream % m_StartPosition;
	}

	template <typename tf_CStream>
	void CDebugManager::CDownloadFile::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_FilePath;
		_Stream % m_FileAttributes;
		_Stream % m_WriteTime;
		_Stream % m_FileSize;
		_Stream % m_SymlinkContents;
		_Stream % fg_Move(m_fGetDataGenerator);
	}

	template <typename tf_CStream>
	void CDebugManager::CFileInfo::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_FileName;
		_Stream % m_Timestamp;
		_Stream % m_Digest;
		_Stream % m_Size;
		_Stream % m_CompressedSize;
	}

	template <typename tf_CStream>
	void CDebugManager::CMetadata::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Product;
		_Stream % m_Application;
		_Stream % m_Configuration;
		_Stream % m_GitBranch;
		_Stream % m_GitCommit;
		_Stream % m_Platform;
		_Stream % m_Version;
	}

	template <typename tf_CStream>
	void CDebugManager::CAssetList::CAsset::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_AssetType;
		_Stream % m_BuildID;
		_Stream % m_FileInfo;
		_Stream % m_Metadata;
		_Stream % m_MainAssetFile;
	}

	template <typename tf_CStream>
	void CDebugManager::CAssetList::CResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % fg_Move(m_AssetsGenerator);
	}

	template <typename tf_CStream>
	void CDebugManager::CAssetFilter::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_AssetType;
		_Stream % m_BuildID;
		_Stream % m_FileName;
		_Stream % m_TimestampStart;
		_Stream % m_TimestampEnd;
		_Stream % m_Metadata;
	}

	template <typename tf_CStream>
	void CDebugManager::CAssetList::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Filter;
	}

	template <typename tf_CStream>
	void CDebugManager::CAssetUpload::CResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % fg_Move(m_fFinish);
	}

	template <typename tf_CStream>
	void CDebugManager::CAssetUpload::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_AssetType;
		_Stream % m_QueueSize;
		_Stream % m_Flags;
		_Stream % fg_Move(m_FilesGenerator);
		_Stream % m_Metadata;
	}

	template <typename tf_CStream>
	void CDebugManager::CAssetDownload::CResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % fg_Move(m_Subscription);
		_Stream % fg_Move(m_FilesGenerator);
	}

	template <typename tf_CStream>
	void CDebugManager::CAssetDownload::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Filter;
		_Stream % fg_Move(m_Subscription);
	}

	template <typename tf_CStream>
	void CDebugManager::CAssetDelete::CResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_nAssetsDeleted;
		_Stream % m_nFilesDeleted;
		_Stream % m_nBytesDeleted;
	}

	template <typename tf_CStream>
	void CDebugManager::CAssetDelete::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Filter;
		_Stream % m_nMaxToDelete;
		_Stream % m_bPretend;
	}

	template <typename tf_CStream>
	void CDebugManager::CCrashDumpList::CCrashDump::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_ID;
		_Stream % m_FileInfo;
		_Stream % m_Metadata;
		_Stream % m_ExceptionInfo;
	}

	template <typename tf_CStream>
	void CDebugManager::CCrashDumpList::CResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % fg_Move(m_CrashDumpsGenerator);
	}

	template <typename tf_CStream>
	void CDebugManager::CCrashDumpFilter::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_ID;
		_Stream % m_FileName;
		_Stream % m_TimestampStart;
		_Stream % m_TimestampEnd;
		_Stream % m_Metadata;
		_Stream % m_ExceptionInfo;
	}
	
	template <typename tf_CStream>
	void CDebugManager::CCrashDumpList::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Filter;
	}

	template <typename tf_CStream>
	void CDebugManager::CCrashDumpUpload::CResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % fg_Move(m_fFinish);
	}

	template <typename tf_CStream>
	void CDebugManager::CCrashDumpUpload::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_ID;
		_Stream % m_QueueSize;
		_Stream % m_Flags ;
		_Stream % fg_Move(m_FilesGenerator);
		_Stream % m_Metadata;
		_Stream % m_ExceptionInfo;
	}

	template <typename tf_CStream>
	void CDebugManager::CCrashDumpDownload::CResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % fg_Move(m_Subscription);
		_Stream % fg_Move(m_FilesGenerator);
	}

	template <typename tf_CStream>
	void CDebugManager::CCrashDumpDownload::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Filter;
		_Stream % fg_Move(m_Subscription);
	}

	template <typename tf_CStream>
	void CDebugManager::CCrashDumpDelete::CResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_nCrashDumpsDeleted;
		_Stream % m_nFilesDeleted;
		_Stream % m_nBytesDeleted;
	}

	template <typename tf_CStream>
	void CDebugManager::CCrashDumpDelete::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Filter;
		_Stream % m_nMaxToDelete;
		_Stream % m_bPretend;
	}
}
