// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManager.h"

namespace NMib::NCloud
{
	CBackupManagerBackup::CBackupManagerBackup()
	{
		DMibPublishActorFunction(CBackupManagerBackup::f_StartBackup);
		DMibPublishActorFunction(CBackupManagerBackup::f_StartRSync);
		DMibPublishActorFunction(CBackupManagerBackup::f_FileChanged);
		DMibPublishActorFunction(CBackupManagerBackup::f_FileRemoved);
		DMibPublishActorFunction(CBackupManagerBackup::f_UploadData);
		DMibPublishActorFunction(CBackupManagerBackup::f_InitialBackupFinished);
	}
	
	auto CBackupManagerBackup::fs_ParseSyncFlags(NEncoding::CEJSON const &_JSON) -> EManifestSyncFlag 
	{
		EManifestSyncFlag Flags = EManifestSyncFlag_None;
		
		for (auto &Flag : _JSON.f_Array())
		{
			if (Flag.f_String() == "Append")
				Flags |= EManifestSyncFlag_Append; 
			else if (Flag.f_String() == "TransactionLog")
				Flags |= EManifestSyncFlag_TransactionLog;
			else
				DMibError(NStr::fg_Format("Unknown sync flag: {}", Flag.f_String()));
		}
		
		return Flags;
	}
	
	NEncoding::CEJSON CBackupManagerBackup::fs_GenerateSyncFlags(EManifestSyncFlag _Flags)
	{
		NEncoding::CEJSON Json;
		Json.f_Array();
		
		if (_Flags & EManifestSyncFlag_Append)
			Json.f_Insert("Append");
		if (_Flags & EManifestSyncFlag_TransactionLog)
			Json.f_Insert("TransactionLog");
		
		return Json;
	}
	
	NStr::CStr const &CBackupManagerBackup::CManifestFile::f_GetFileName() const
	{
		return NContainer::TCMap<NStr::CStr, CManifestFile>::fs_GetKey(*this);
	}
	
	template <typename tf_CStream>
	void CBackupManagerBackup::CManifestFile::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Digest;
		_Stream % m_Length;
		_Stream % m_WriteTime;
		_Stream % m_SymlinkData;
		_Stream % m_Attributes;
		_Stream % m_Owner;
		_Stream % m_Group;
		_Stream % m_Flags;
	}
	DMibDistributedStreamImplement(CBackupManagerBackup::CManifestFile);
	
	template <typename tf_CStream>
	void CBackupManagerBackup::CManifest::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Files;
	}
	DMibDistributedStreamImplement(CBackupManagerBackup::CManifest);
	
	template <typename tf_CStream>
	void CBackupManagerBackup::CStartBackupResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_FilesNotUpToDate;
	}
	DMibDistributedStreamImplement(CBackupManagerBackup::CStartBackupResult);
		
	CBackupManager::CBackupManager()
	{
		DMibPublishActorFunction(CBackupManager::f_StartBackup);
		DMibPublishActorFunction(CBackupManager::f_StopBackup);
		DMibPublishActorFunction(CBackupManager::f_UploadData);
		DMibPublishActorFunction(CBackupManager::f_InitBackup);
		DMibPublishActorFunction(CBackupManager::f_ListBackupSources);
		DMibPublishActorFunction(CBackupManager::f_ListBackups);
		DMibPublishActorFunction(CBackupManager::f_StartDownloadBackup);
	}
	
	auto CBackupManager::f_StartBackup(CStartBackup &&_Params) -> NConcurrency::TCContinuation<CStartBackup::CResult> 
	{
		return DMibErrorInstance("Deprecated");
	}
	
	auto CBackupManager::f_StopBackup(CStopBackup &&_Params) -> NConcurrency::TCContinuation<CStopBackup::CResult> 
	{
		return DMibErrorInstance("Deprecated");
	}
	
	auto CBackupManager::f_UploadData(CUploadData &&_Params) -> NConcurrency::TCContinuation<CUploadData::CResult> 
	{
		return DMibErrorInstance("Deprecated");
	}
	
	bool CBackupManager::fs_IsValidHostname(NStr::CStr const &_String)
	{
		return NNet::fg_IsValidHostname(_String);
	}
	
	bool CBackupManager::fs_IsValidBackupSource(NStr::CStr const &_String, NStr::CStr *o_pFriendlyName, NStr::CStr *o_pHostID)
	{
		NStr::CStr FriendlyName;
		NStr::CStr HostID;
		aint nParsed = 0;
		aint nCharsParsed = (NStr::CStr::CParse("{}_{}") >> FriendlyName >> HostID).f_Parse(_String, nParsed);
		if (nCharsParsed != _String.f_GetLen() || nParsed != 2)
		{
			nCharsParsed = (NStr::CStr::CParse("{} - {}") >> FriendlyName >> HostID).f_Parse(_String, nParsed);
			if (nCharsParsed != _String.f_GetLen() || nParsed != 2)
				return false;
		}
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
		aint nCharsParsed = (NStr::CStr::CParse("{}-{}-{}_{}.{}.{}.{}_{}") >> Year >> Month >> Day >> Hour >> Minute >> Second >> Millisecond >> BackupID).f_Parse(_String, nParsed);
		if (nCharsParsed != _String.f_GetLen() || nParsed != 8)
		{
			nCharsParsed = (NStr::CStr::CParse("{}-{}-{} {}.{}.{}.{} - {}") >> Year >> Month >> Day >> Hour >> Minute >> Second >> Millisecond >> BackupID).f_Parse(_String, nParsed);
			if (nCharsParsed != _String.f_GetLen() || nParsed != 8)
				return false;
		}
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
	
	template <typename tf_CStream>
	void CBackupManager::CBackupKey::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_FriendlyName;
		_Stream % m_Time;
		_Stream % m_ID;
	}
	DMibDistributedStreamImplement(CBackupManager::CBackupKey);
	
	// CStartBackup
	
	template <typename tf_CStream>
	void CBackupManager::CStartBackup::CResult::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_FriendlyName;
		_Stream % m_BackupSize;
		_Stream % m_OplogSize;
	}
	DMibDistributedStreamImplement(CBackupManager::CStartBackup::CResult);

	template <typename tf_CStream>
	void CBackupManager::CStartBackup::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_BackupKey;
	}
	DMibDistributedStreamImplement(CBackupManager::CStartBackup);

	// CStopBackup
	
	template <typename tf_CStream>
	void CBackupManager::CStopBackup::CResult::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}
	DMibDistributedStreamImplement(CBackupManager::CStopBackup::CResult);

	template <typename tf_CStream>
	void CBackupManager::CStopBackup::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_BackupKey;
	}
	DMibDistributedStreamImplement(CBackupManager::CStopBackup);

	// CUploadData
	
	template <typename tf_CStream>
	void CBackupManager::CUploadData::CResult::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}
	DMibDistributedStreamImplement(CBackupManager::CUploadData::CResult);

	template <typename tf_CStream>
	void CBackupManager::CUploadData::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_BackupKey;
		_Stream % m_File;
		_Stream % m_Position;
		_Stream % m_Size;
		_Stream % m_Flags;
		_Stream % m_Data;
	}
	DMibDistributedStreamImplement(CBackupManager::CUploadData);
	
	// CListBackupSources
	
	template <typename tf_CStream>
	void CBackupManager::CListBackupSources::CResult::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_BackupSources;
	}
	DMibDistributedStreamImplement(CBackupManager::CListBackupSources::CResult);
	
	template <typename tf_CStream>
	void CBackupManager::CListBackupSources::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}
	DMibDistributedStreamImplement(CBackupManager::CListBackupSources);

	// CListBackups
	
	void CBackupManager::CListBackups::CResult::f_Format(NStr::CStrAggregate &o_Str) const
	{
		o_Str += NStr::CStr::CFormat("{vs}") << m_Backups;
	}

	void CBackupManager::CListBackups::CBackup::f_Format(NStr::CStrAggregate &o_Str) const
	{
		o_Str += NStr::CStr::CFormat("{tst.,tsb_}_{}") << m_Time << m_BackupID;
	}

	template <typename tf_CStream>
	void CBackupManager::CListBackups::CResult::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_Backups;
	}
	DMibDistributedStreamImplement(CBackupManager::CListBackups::CResult);

	template <typename tf_CStream>
	void CBackupManager::CListBackups::CBackup::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Time;
		_Stream % m_BackupID;
	}
	DMibDistributedStreamImplement(CBackupManager::CListBackups::CBackup);
	
	template <typename tf_CStream>
	void CBackupManager::CListBackups::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_ForBackupSource;
	}
	DMibDistributedStreamImplement(CBackupManager::CListBackups);
	
	// CStartDownloadBackup

	template <typename tf_CStream>
	void CBackupManager::CStartDownloadBackup::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_BackupSource;
		_Stream % m_Time;
		_Stream % m_BackupID;
		_Stream % m_TransferContext;
	}
	DMibDistributedStreamImplement(CBackupManager::CStartDownloadBackup);
	
	NStr::CStr CBackupManager::CStartDownloadBackup::f_GetDesc() const
	{
		return fg_Format
			(
				"{}/{tst.,tsb_}_{}"
				, m_BackupSource
				, m_Time
				, m_BackupID
			)
		;
	}
	
	template <typename tf_CStream>
	void CBackupManager::CStartDownloadBackup::CResult::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % fg_Move(m_Subscription);
	}
	DMibDistributedStreamImplement(CBackupManager::CStartDownloadBackup::CResult);
}
