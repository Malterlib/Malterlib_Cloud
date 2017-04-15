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
		m_ManifestFile.f_Stream(_Stream, 0x101);
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
}
