// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManager.h"

namespace NMib::NCloud
{
	bool CBackupManager::fs_IsValidHostname(NStr::CStr const &_String)
	{
		return NNet::fg_IsValidHostname(_String);
	}
	
	bool CBackupManager::fs_IsValidBackupSource(NStr::CStr const &_String, NStr::CStr *o_pFriendlyName, NStr::CStr *o_pHostID)
	{
		NStr::CStr FriendlyName;
		NStr::CStr HostID;
		aint nParsed = 0;
		aint nCharsParsed = (NStr::CStr::CParse("{} - {}") >> FriendlyName >> HostID).f_Parse(_String, nParsed);
		if (nCharsParsed != _String.f_GetLen() || nParsed != 2)
			return false;
		if (!fs_IsValidHostname(FriendlyName))
			return false;
		if (!fs_IsValidHostname(HostID))
			return false;
		if (o_pFriendlyName)
			*o_pFriendlyName = FriendlyName; 
		if (o_pHostID)
			*o_pHostID = HostID; 
		return true;
	}
	
	bool CBackupManager::fs_IsValidBackup(NStr::CStr const &_String, NStr::CStr *o_pBackupID, NTime::CTime *o_pTime)
	{
		if (_String == "Latest")
		{
			if (o_pBackupID)
				*o_pBackupID = "Latest";
			if (o_pTime)
				*o_pTime = NTime::CTime();
			return true;
		}
		NStr::CStr Year;
		NStr::CStr Month;
		NStr::CStr Day;
		NStr::CStr Hour;
		NStr::CStr Minute;
		NStr::CStr Second;
		NStr::CStr Millisecond;
		NStr::CStr BackupID;
		aint nParsed = 0;
		aint nCharsParsed = (NStr::CStr::CParse("{}-{}-{} {}.{}.{}.{} - {}") >> Year >> Month >> Day >> Hour >> Minute >> Second >> Millisecond >> BackupID).f_Parse(_String, nParsed);
		if (nCharsParsed != _String.f_GetLen() || nParsed != 8)
			return false;
		if 
			(
				!Year.f_IsNumeric() 
				|| !Month.f_IsNumeric()
				|| !Day.f_IsNumeric()
				|| !Hour.f_IsNumeric()
				|| !Minute.f_IsNumeric()
				|| !Second.f_IsNumeric()
				|| !Millisecond.f_IsNumeric()
			)
		{
			return false;
		}
		if (!fs_IsValidHostname(BackupID))
			return false;
		
		if (o_pBackupID)
			*o_pBackupID = BackupID;
		if (o_pTime)
		{
			*o_pTime = NTime::CTimeConvert::fs_CreateTime
				(
					fg_Max(Year.f_ToInt(int64(1970)), int64(1970))
					, fg_Clamp(Month.f_ToInt(uint32(1)), 1u, 12u)
					, fg_Clamp(Day.f_ToInt(uint32(1)), 1u, 31u)
					, fg_Clamp(Hour.f_ToInt(uint32(0)), 0u, 23u)
					, fg_Clamp(Minute.f_ToInt(uint32(0)), 0u, 59u)
					, fg_Clamp(Second.f_ToInt(uint32(0)), 0u, 59u)
					, fg_Clamp(("0." + Millisecond).f_ToFloat(fp64(0.0)), 0.0, 0.999)
				)
			;
		}
		return true;
	}	

	bool CBackupManager::fs_IsValidProtocolVersion(uint32 _Version)
	{
		return _Version >= EMinProtocolVersion && _Version <= EProtocolVersion;
	}
	
	void CBackupManager::CBackupKey::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_FriendlyName;
		_Stream << m_Time;
		_Stream << m_ID;
	}
	void CBackupManager::CBackupKey::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_FriendlyName;
		_Stream >> m_Time;
		_Stream >> m_ID;
	}
	
	// CStartBackup
	
	void CBackupManager::CStartBackup::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_FriendlyName;
		_Stream << m_BackupSize;
		_Stream << m_OplogSize;
	}
	
	void CBackupManager::CStartBackup::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_FriendlyName;
		_Stream >> m_BackupSize;
		_Stream >> m_OplogSize;
	}

	void CBackupManager::CStartBackup::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_BackupKey;
	}
	
	void CBackupManager::CStartBackup::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_BackupKey;
	}

	// CStopBackup
	
	void CBackupManager::CStopBackup::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}
	
	void CBackupManager::CStopBackup::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}

	void CBackupManager::CStopBackup::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_BackupKey;
	}
	
	void CBackupManager::CStopBackup::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_BackupKey;
	}

	// CUploadData
	
	void CBackupManager::CUploadData::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}
	
	void CBackupManager::CUploadData::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}

	void CBackupManager::CUploadData::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_BackupKey;
		_Stream << m_File;
		_Stream << m_Position;
		_Stream << m_Size;
		_Stream << m_Flags;
		_Stream << m_Data;
	}
	
	void CBackupManager::CUploadData::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_BackupKey;
		_Stream >> m_File;
		_Stream >> m_Position;
		_Stream >> m_Size;
		_Stream >> m_Flags;
		_Stream >> m_Data;
	}

	// CListBackupSources
	
	void CBackupManager::CListBackupSources::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_BackupSources;
	}
	void CBackupManager::CListBackupSources::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_BackupSources;
	}
	
	void CBackupManager::CListBackupSources::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}
	
	void CBackupManager::CListBackupSources::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}

	// CListBackups
	
	void CBackupManager::CListBackups::CResult::f_Format(NStr::CStrAggregate &o_Str) const
	{
		o_Str += NStr::CStr::CFormat("{vs}") << m_Backups;
	}

	void CBackupManager::CListBackups::CBackup::f_Format(NStr::CStrAggregate &o_Str) const
	{
		o_Str += NStr::CStr::CFormat("{tst.} - {}") << m_Time << m_BackupID;
	}

	void CBackupManager::CListBackups::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_Backups;
	}
	
	void CBackupManager::CListBackups::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_Backups;
	}

	void CBackupManager::CListBackups::CBackup::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_Time;
		_Stream << m_BackupID;
	}
	
	void CBackupManager::CListBackups::CBackup::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Time;
		_Stream >> m_BackupID;
	}
	
	void CBackupManager::CListBackups::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_ForBackupSource;
	}
	
	void CBackupManager::CListBackups::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_ForBackupSource;
	}
	
	// CStartDownloadBackup

	void CBackupManager::CStartDownloadBackup::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_BackupSource;
		_Stream << m_Time;
		_Stream << m_BackupID;
		_Stream << m_TransferContext;
	}
	
	void CBackupManager::CStartDownloadBackup::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_BackupSource;
		_Stream >> m_Time;
		_Stream >> m_BackupID;
		_Stream >> m_TransferContext;
	}
	
	NStr::CStr CBackupManager::CStartDownloadBackup::f_GetDesc() const
	{
		return fg_Format
			(
				"{}/{tst.} - {}"
				, m_BackupSource
				, m_Time
				, m_BackupID
			)
		;
	}
	
	void CBackupManager::CStartDownloadBackup::CResult::f_Feed(CDistributedActorWriteStream &_Stream) &&
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << fg_Move(m_Subscription);
	}
	
	void CBackupManager::CStartDownloadBackup::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_Subscription;
	}
}
