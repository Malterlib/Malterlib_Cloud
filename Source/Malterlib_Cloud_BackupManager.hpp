// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void CBackupManagerBackup::CStartBackupResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_FilesNotUpToDate;
	}
	
	template <typename tf_CStream>
	void CBackupManagerBackup::CManifestChange_Change::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_ManifestFile;
	}

	template <typename tf_CStream>
	void CBackupManagerBackup::CManifestFile::f_Stream(tf_CStream &_Stream)
	{
		NFile::CDirectoryManifestFile::f_Stream(_Stream, 0x102);
	}

	inline NFile::CDirectoryManifestFile &CBackupManagerBackup::CManifestFile::f_ManifestFile()
	{
		return *this;
	}

	template <typename tf_CStream>
	void CBackupManagerBackup::CAppendData::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Position;
		_Stream % m_Data;
		_Stream % m_PreviousDigest;
		_Stream % m_ManifestFile;
	}
	
	template <typename tf_CStream>
	void CBackupManagerBackup::CManifestChange_Remove::f_Stream(tf_CStream &_Stream)
	{
	}
	
	template <typename tf_CStream>
	void CBackupManagerBackup::CManifestChange_Rename::f_Stream(tf_CStream &_Stream)
	{
		CManifestChange_Change::f_Stream(_Stream);
		_Stream % m_FromFileName;
	}

	template <typename tf_CStream>
	void CBackupManagerBackup::CInitialBackupFinishedResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_AddedFiles;
		_Stream % m_RemovedFiles;
		_Stream % m_UpdatedFiles;
	}
}
