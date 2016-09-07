// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManager.h"

namespace NMib::NCloud
{
	
	void CBackupManager::CBackupKey::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		_Stream << m_FriendlyName;
		_Stream << m_Time;
		_Stream << m_ID;
	}
	void CBackupManager::CBackupKey::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_FriendlyName;
		_Stream >> m_Time;
		_Stream >> m_ID;
	}
	
	// CStartBackup
	
	auto CBackupManager::CStartBackup::f_GetResult() const -> CResult
	{
		CResult Result;
		Result.m_Version = m_Version;
		return Result;
	}
	
	void CBackupManager::CStartBackup::CResult::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
		_Stream << m_FriendlyName;
		_Stream << m_BackupSize;
		_Stream << m_OplogSize;
	}
	
	void CBackupManager::CStartBackup::CResult::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
		_Stream >> m_FriendlyName;
		_Stream >> m_BackupSize;
		_Stream >> m_OplogSize;
	}

	void CBackupManager::CStartBackup::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
		_Stream << m_BackupKey;
	}
	
	void CBackupManager::CStartBackup::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
		_Stream >> m_BackupKey;
	}

	// CStopBackup
	
	auto CBackupManager::CStopBackup::f_GetResult() const -> CResult
	{
		CResult Result;
		Result.m_Version = m_Version;
		return Result;
	}
	
	void CBackupManager::CStopBackup::CResult::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
	}
	
	void CBackupManager::CStopBackup::CResult::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
	}

	void CBackupManager::CStopBackup::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
		_Stream << m_BackupKey;
	}
	
	void CBackupManager::CStopBackup::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
	}

	// CUploadData
	
	auto CBackupManager::CUploadData::f_GetResult() const -> CResult
	{
		CResult Result;
		Result.m_Version = m_Version;
		return Result;
	}
	
	void CBackupManager::CUploadData::CResult::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
	}
	
	void CBackupManager::CUploadData::CResult::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
	}

	void CBackupManager::CUploadData::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
		_Stream << m_BackupKey;
		_Stream << m_File;
		_Stream << m_Position;
		_Stream << m_Size;
		_Stream << m_Flags;
		_Stream << m_Data;
	}
	
	void CBackupManager::CUploadData::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
		_Stream >> m_BackupKey;
		_Stream >> m_File;
		_Stream >> m_Position;
		_Stream >> m_Size;
		_Stream >> m_Flags;
		_Stream >> m_Data;
	}

	// CListBackupSources
	
	auto CBackupManager::CListBackupSources::f_GetResult() const -> CResult
	{
		CResult Result;
		Result.m_Version = m_Version;
		return Result;
	}
	
	void CBackupManager::CListBackupSources::CResult::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
		_Stream << m_BackupSources;
	}
	void CBackupManager::CListBackupSources::CResult::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
		_Stream >> m_BackupSources;
	}
	
	void CBackupManager::CListBackupSources::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
	}
	
	void CBackupManager::CListBackupSources::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
	}

	// CListBackups
	
	auto CBackupManager::CListBackups::f_GetResult() const -> CResult
	{
		CResult Result;
		Result.m_Version = m_Version;
		return Result;
	}
	
	void CBackupManager::CListBackups::CResult::f_Format(NStr::CStrAggregate &o_Str) const
	{
		o_Str += NStr::CStr::CFormat("{vs}") << m_Backups;
	}

	void CBackupManager::CListBackups::CBackup::f_Format(NStr::CStrAggregate &o_Str) const
	{
		o_Str += NStr::CStr::CFormat("{} - {}") << m_Time << m_BackupID;
	}

	void CBackupManager::CListBackups::CResult::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
		_Stream << m_Backups;
	}
	
	void CBackupManager::CListBackups::CResult::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
		_Stream >> m_Backups;
	}

	void CBackupManager::CListBackups::CBackup::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		_Stream << m_Time;
		_Stream << m_BackupID;
	}
	
	void CBackupManager::CListBackups::CBackup::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_Time;
		_Stream >> m_BackupID;
	}
	
	void CBackupManager::CListBackups::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
		_Stream << m_ForBackupSource;
	}
	
	void CBackupManager::CListBackups::f_Consume(NStream::CBinaryStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
		_Stream >> m_ForBackupSource;
	}
}
