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
	
	void CBackupManager::CStartBackup::CResult::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		_Stream << EProtocolVersion;
		_Stream << m_FriendlyName;
		_Stream << m_BackupSize;
		_Stream << m_OplogSize;
	}
	void CBackupManager::CStartBackup::CResult::f_Consume(NStream::CBinaryStream &_Stream)
	{
		uint32 Version;
		_Stream >> Version;
		if (Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, Version);
		_Stream >> m_FriendlyName;
		_Stream >> m_BackupSize;
		_Stream >> m_OplogSize;
	}

	void CBackupManager::CStartBackup::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		_Stream << EProtocolVersion;
		_Stream << m_BackupKey;
	}
	
	void CBackupManager::CStartBackup::f_Consume(NStream::CBinaryStream &_Stream)
	{
		uint32 Version;
		_Stream >> Version;
		if (Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, Version);
		_Stream >> m_BackupKey;
	}

	void CBackupManager::CStopBackup::CResult::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		_Stream << EProtocolVersion;
	}
	void CBackupManager::CStopBackup::CResult::f_Consume(NStream::CBinaryStream &_Stream)
	{
		uint32 Version;
		_Stream >> Version;
		if (Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, Version);
	}

	void CBackupManager::CStopBackup::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		_Stream << EProtocolVersion;
		_Stream << m_BackupKey;
	}
	
	void CBackupManager::CStopBackup::f_Consume(NStream::CBinaryStream &_Stream)
	{
		uint32 Version;
		_Stream >> Version;
		if (Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, Version);
		_Stream >> m_BackupKey;
	}
	
	void CBackupManager::CUploadData::CResult::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		_Stream << EProtocolVersion;
	}
	void CBackupManager::CUploadData::CResult::f_Consume(NStream::CBinaryStream &_Stream)
	{
		uint32 Version;
		_Stream >> Version;
		if (Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, Version);
	}

	void CBackupManager::CUploadData::f_Feed(NStream::CBinaryStream &_Stream) const
	{
		_Stream << EProtocolVersion;
		_Stream << m_BackupKey;
		_Stream << m_File;
		_Stream << m_Position;
		_Stream << m_Size;
		_Stream << m_Flags;
		_Stream << m_Data;
	}
	
	void CBackupManager::CUploadData::f_Consume(NStream::CBinaryStream &_Stream)
	{
		uint32 Version;
		_Stream >> Version;
		if (Version > EProtocolVersion)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, Version);
		_Stream >> m_BackupKey;
		_Stream >> m_File;
		_Stream >> m_Position;
		_Stream >> m_Size;
		_Stream >> m_Flags;
		_Stream >> m_Data;
	}
}
