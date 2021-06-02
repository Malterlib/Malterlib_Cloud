// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManager.h"

namespace NMib::NCloud
{
	DMibImpErrorClassImplement(CExceptionBackupManagerHashMismatch);

	CBackupManagerBackup::CManifestChange_Change::CManifestChange_Change() = default;

	CBackupManagerBackup::CManifestChange_Change::CManifestChange_Change(CManifestFile const &_ManifestFile)
		: m_ManifestFile(_ManifestFile)
	{
	}

	CBackupManagerBackup::CManifestChange_Add::CManifestChange_Add(NFile::CDirectoryManifestFile &&_ManifestFile)
		: CManifestChange_Change{_ManifestFile}
	{
	}

	CBackupManagerBackup::CManifestChange_Rename::CManifestChange_Rename(NFile::CDirectoryManifestFile &&_ManifestFile, NStr::CStr const &_FromFileName)
		: CManifestChange_Change{_ManifestFile}
		, m_FromFileName{_FromFileName}
	{
	}

	CBackupManagerBackup::CManifestFile::CManifestFile() = default;

	CBackupManagerBackup::CManifestFile::CManifestFile(NFile::CDirectoryManifestFile const &_ManifestFile)
		: NFile::CDirectoryManifestFile{_ManifestFile}
	{
	}

	CBackupManagerBackup::CBackupManagerBackup()
	{
		DMibPublishActorFunction(CBackupManagerBackup::f_StartManifestRSync);
		DMibPublishActorFunction(CBackupManagerBackup::f_StartBackup);
		DMibPublishActorFunction(CBackupManagerBackup::f_StartRSync);
		DMibPublishActorFunction(CBackupManagerBackup::f_ManifestChange);
		DMibPublishActorFunction(CBackupManagerBackup::f_AppendData);
		DMibPublishActorFunction(CBackupManagerBackup::f_InitialBackupFinished);
		DMibConcurrencyRegisterException(CExceptionBackupManagerHashMismatch);
	}

	namespace
	{
		bool fg_IsFileNameValid(NStr::CStr const &_FileName, NStr::CStr &o_Error, ch8 const *_pDesc)
		{
			NStr::CStr Error;
			if (!NFile::CFile::fs_IsSafeRelativePath(_FileName, Error))
			{
				o_Error = NStr::fg_Format("{} cannot {}: '{}'", _pDesc, Error, _FileName);
				return false;
			}
			return true;
		}
	}

	bool CBackupManagerBackup::fs_ManifestFileValid(NStr::CStr const &_FileName, NFile::CDirectoryManifestFile const &_File, NStr::CStr &o_Error)
	{
		if (!fg_IsFileNameValid(_FileName, o_Error, "File name"))
			return false;

		if (_File.f_IsFile())
		{
			if (_File.m_OriginalPath.f_IsEmpty())
			{
				o_Error = "Original path cannot be empty for files";
				return false;
			}
			if (!fg_IsFileNameValid(_File.m_OriginalPath, o_Error, "Original path"))
				return false;
		}
		if (_File.m_Attributes & NFile::EFileAttrib_Link)
		{
			NStr::CStr Error;
			if (!NFile::CFile::fs_IsValidFilePath(_File.m_SymlinkData, Error))
			{
				o_Error = NStr::fg_Format("Symlink data '{}' cannot be {}", _File.m_SymlinkData, Error);
				return false;
			}
		}
		return true;
	}

	bool CBackupManagerBackup::fs_ManifestChangeValid(NStr::CStr const &_FileName, CManifestChange const &_Change, NStr::CStr &o_Error)
	{
		if (!fg_IsFileNameValid(_FileName, o_Error, "File name"))
			return false;
		switch (_Change.f_GetTypeID())
		{
		case EManifestChange_Add:
			{
				auto &Change = _Change.f_Get<EManifestChange_Add>();
				return CBackupManagerBackup::fs_ManifestFileValid(_FileName, Change.m_ManifestFile, o_Error);
			}
			break;
		case EManifestChange_Change:
			{
				auto &Change = _Change.f_Get<EManifestChange_Change>();
				return CBackupManagerBackup::fs_ManifestFileValid(_FileName, Change.m_ManifestFile, o_Error);
			}
			break;
		case EManifestChange_Rename:
			{
				auto &Change = _Change.f_Get<EManifestChange_Rename>();
				if (!fg_IsFileNameValid(Change.m_FromFileName, o_Error, "Rename from file name"))
					return false;
				return CBackupManagerBackup::fs_ManifestFileValid(_FileName, Change.m_ManifestFile, o_Error);
			}
			break;
		case EManifestChange_Remove:
			break;
		}
		return true;
	}

	NFile::CDirectoryManifestFile const &CBackupManagerBackup::fs_ManifestFileFromChange(CManifestChange const &_Change)
	{
		switch (_Change.f_GetTypeID())
		{
		case EManifestChange_Add:
			{
				auto &Change = _Change.f_Get<EManifestChange_Add>();
				return Change.m_ManifestFile;
			}
			break;
		case EManifestChange_Change:
			{
				auto &Change = _Change.f_Get<EManifestChange_Change>();
				return Change.m_ManifestFile;
			}
			break;
		case EManifestChange_Rename:
			{
				auto &Change = _Change.f_Get<EManifestChange_Rename>();
				return Change.m_ManifestFile;
			}
			break;
		case EManifestChange_Remove:
			break;
		}
		DMibError("Invalid change type");
	}

	bool CBackupManagerBackup::fs_PretendApplyManifestChange(NFile::CDirectoryManifest const &_Manifest, NStr::CStr const &_FileName, CManifestChange const &_Change)
	{
		bool bChanged = false;

		switch (_Change.f_GetTypeID())
		{
		case EManifestChange_Add:
			{
				auto &Change = _Change.f_Get<EManifestChange_Add>();
				if (auto pPrevious = _Manifest.m_Files.f_FindEqual(_FileName))
					bChanged = *pPrevious != Change.m_ManifestFile;
				else
					bChanged = true;
			}
			break;
		case EManifestChange_Change:
			{
				auto &Change = _Change.f_Get<EManifestChange_Change>();
				if (auto pPrevious = _Manifest.m_Files.f_FindEqual(_FileName))
					bChanged = *pPrevious != Change.m_ManifestFile;
				else
					bChanged = true;
			}
			break;
		case EManifestChange_Remove:
			{
				bChanged = _Manifest.m_Files.f_FindEqual(_FileName);
			}
			break;
		case EManifestChange_Rename:
			{
				bChanged = true;
			}
			break;
		default:
			{
				DMibNeverGetHere;
			}
			break;
		}

		return bChanged;
	}

	bool CBackupManagerBackup::fs_ApplyManifestChange(NFile::CDirectoryManifest &o_Manifest, NStr::CStr const &_FileName, CManifestChange const &_Change)
	{
		bool bChanged = false;

		switch (_Change.f_GetTypeID())
		{
		case EManifestChange_Add:
			{
				auto &Change = _Change.f_Get<EManifestChange_Add>();

				if (auto pPrevious = o_Manifest.m_Files.f_FindEqual(_FileName))
					bChanged = *pPrevious != Change.m_ManifestFile;
				else
					bChanged = true;

				o_Manifest.m_Files[_FileName] = Change.m_ManifestFile;
			}
			break;
		case EManifestChange_Change:
			{
				auto &Change = _Change.f_Get<EManifestChange_Change>();

				if (auto pPrevious = o_Manifest.m_Files.f_FindEqual(_FileName))
					bChanged = *pPrevious != Change.m_ManifestFile;
				else
					bChanged = true;

				o_Manifest.m_Files[_FileName] = Change.m_ManifestFile;
			}
			break;
		case EManifestChange_Remove:
			{
				bChanged = o_Manifest.m_Files.f_Remove(_FileName);
			}
			break;
		case EManifestChange_Rename:
			{
				auto &Change = _Change.f_Get<EManifestChange_Rename>();

				bChanged = true;

				o_Manifest.m_Files.f_Remove(Change.m_FromFileName);
				o_Manifest.m_Files[_FileName] = Change.m_ManifestFile;
			}
			break;
		default:
			{
				DMibNeverGetHere;
			}
			break;
		}

		return bChanged;
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
	
	auto CBackupManager::f_StartBackup(CStartBackup &&_Params) -> NConcurrency::TCFuture<CStartBackup::CResult>
	{
		co_return DMibErrorInstance("Deprecated");
	}
	
	auto CBackupManager::f_StopBackup(CStopBackup &&_Params) -> NConcurrency::TCFuture<CStopBackup::CResult>
	{
		co_return DMibErrorInstance("Deprecated");
	}
	
	auto CBackupManager::f_UploadData(CUploadData &&_Params) -> NConcurrency::TCFuture<CUploadData::CResult>
	{
		co_return DMibErrorInstance("Deprecated");
	}
	
	auto CBackupManager::f_StartDownloadBackup(CStartDownloadBackup &&_Params) -> NConcurrency::TCFuture<CStartDownloadBackup::CResult>
	{
		co_return DMibErrorInstance("Deprecated");
	}
	
	bool CBackupManager::fs_IsValidHostname(NStr::CStr const &_String)
	{
		return NNetwork::fg_IsValidHostname(_String);
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

	bool CBackupManager::fs_IsValidBackupTime(NStr::CStr const &_String, NTime::CTime *o_pTime)
	{
		NStr::CStr Year;
		NStr::CStr Month;
		NStr::CStr Day;
		NStr::CStr Hour;
		NStr::CStr Minute;
		NStr::CStr Second;
		NStr::CStr Millisecond;
		aint nParsed = 0;
		aint nCharsParsed = (NStr::CStr::CParse("{}-{}-{}_{}.{}.{}.{}") >> Year >> Month >> Day >> Hour >> Minute >> Second >> Millisecond).f_Parse(_String, nParsed);
		if (nCharsParsed != _String.f_GetLen() || nParsed != 7)
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

	void CBackupManager::CBackupInfo::f_Format(NStr::CStrAggregate &o_Str) const
	{
		using namespace NStr;
		
		o_Str += "{tc5} -> {tc5}"_f << m_Earliest << m_Latest;
	}

	void CBackupManager::CBackupID::f_Format(NStr::CStrAggregate &o_Str) const
	{
		if (m_Time.f_IsValid())
			o_Str += NStr::CStr::CFormat("{tst.,tsb_}_{}") << m_Time << m_ID;
		else
			o_Str += NStr::CStr::CFormat("{}") << m_ID;
	}

	bool CBackupManager::CBackupID::operator < (CBackupID const &_Right) const
	{
		return NStorage::fg_TupleReferences(m_Time, m_ID) < NStorage::fg_TupleReferences(_Right.m_Time, _Right.m_ID);
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
	
	// CInitBackup

	template <typename tf_CStream>
	void CBackupManager::CInitBackup::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		DMibRequire(_Stream.f_GetVersion() >= 0x103);
		_Stream % m_BackupKey;
		_Stream % fg_Move(m_Subscription);
		_Stream % m_Flags;
	}
	DMibDistributedStreamImplement(CBackupManager::CInitBackup);

	NStr::CStr CBackupManager::CDownloadBackup::f_GetDesc() const
	{
		using namespace NStr;

		if (m_Time.f_IsValid())
			return "{}/{}"_f << m_BackupSource << m_Time;
		else
			return "{}/Latest"_f << m_BackupSource;
	}

	template <typename tf_CStream>
	void CBackupManager::CDownloadBackup::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		DMibRequire(_Stream.f_GetVersion() >= 0x103);
		_Stream % m_BackupSource;
		_Stream % m_Time;
		_Stream % fg_Move(m_Subscription);
	}
	DMibDistributedStreamImplement(CBackupManager::CDownloadBackup);

	template <typename tf_CStream>
	void CBackupManager::CBackupInfo::f_Stream(tf_CStream &_Stream)
	{
		DMibRequire(_Stream.f_GetVersion() >= 0x103);
		_Stream % m_Earliest;
		_Stream % m_Latest;
		_Stream % m_Snapshots;
	}
	DMibDistributedStreamImplement(CBackupManager::CBackupInfo);

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
