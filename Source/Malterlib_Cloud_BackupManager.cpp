// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManager.h"

namespace NMib::NCloud
{
	CBackupManagerBackup::CBackupManagerBackup()
	{
		DMibPublishActorFunction(CBackupManagerBackup::f_StartManifestRSync);
		DMibPublishActorFunction(CBackupManagerBackup::f_StartBackup);
		DMibPublishActorFunction(CBackupManagerBackup::f_StartRSync);
		DMibPublishActorFunction(CBackupManagerBackup::f_ManifestChange);
		DMibPublishActorFunction(CBackupManagerBackup::f_UploadData);
		DMibPublishActorFunction(CBackupManagerBackup::f_InitialBackupFinished);
	}
	
	DMibDistributedStreamImplement(CBackupManagerBackup::CStartBackupResult);
	DMibDistributedStreamImplement(CBackupManagerBackup::CManifestChange_Change);
	DMibDistributedStreamImplement(CBackupManagerBackup::CManifestChange_Remove);
	DMibDistributedStreamImplement(CBackupManagerBackup::CManifestChange_Rename);

	CBackupManager::CBackupManager()
	{
		DMibPublishActorFunction(CBackupManager::f_StartBackup);
		DMibPublishActorFunction(CBackupManager::f_StopBackup);
		DMibPublishActorFunction(CBackupManager::f_UploadData);
		DMibPublishActorFunction(CBackupManager::f_InitBackup);
		DMibPublishActorFunction(CBackupManager::f_ListBackupSources);
		DMibPublishActorFunction(CBackupManager::f_ListBackups);
		DMibPublishActorFunction(CBackupManager::f_StartDownloadBackup);
		DMibPublishActorFunction(CBackupManager::f_DownloadBackup);
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
	
	auto CBackupManager::f_StartDownloadBackup(CStartDownloadBackup &&_Params) -> NConcurrency::TCContinuation<CStartDownloadBackup::CResult>
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
	
	void CBackupManager::CBackupID::f_Format(NStr::CStrAggregate &o_Str) const
	{
		o_Str += NStr::CStr::CFormat("{tst.,tsb_}_{}") << m_Time << m_ID;
	}

	template <typename tf_CStream>
	void CBackupManager::CBackupID::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Time;
		_Stream % m_ID;
	}
	DMibDistributedStreamImplement(CBackupManager::CBackupID);
	
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
	
	// CStartDownloadBackup

	template <typename tf_CStream>
	void CBackupManager::CStartDownloadBackup::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_BackupSource;
		_Stream % m_BackupID;
		_Stream % m_TransferContext;
	}
	DMibDistributedStreamImplement(CBackupManager::CStartDownloadBackup);
	
	NStr::CStr CBackupManager::CStartDownloadBackup::f_GetDesc() const
	{
		return fg_Format
			(
				"{}/{}"
				, m_BackupSource
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
