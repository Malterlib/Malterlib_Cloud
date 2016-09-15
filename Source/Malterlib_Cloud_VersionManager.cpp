// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_VersionManager.h"

namespace NMib::NCloud
{
	using namespace NStr;
	
	bool CVersionManager::fs_IsValidApplicationName(CStr const &_String)
	{
		return NNet::fg_IsValidHostname(_String);
	}
	
	bool CVersionManager::fs_IsValidBranch(NStr::CStr const &_String)
	{
		return NNet::fg_IsValidHostname(_String, "/");
	}

	bool CVersionManager::fs_IsValidTag(CStr const &_String)
	{
		return NNet::fg_IsValidHostname(_String);
	}
	
	bool CVersionManager::fs_IsValidProtocolVersion(uint32 _Version)
	{
		return _Version >= EMinProtocolVersion && _Version <= EProtocolVersion;
	}
	
	bool CVersionManager::fs_IsValidVersionIdentifier(CVersionIdentifier const &_VersionID, CStr &o_Error)
	{
		if (!fs_IsValidBranch(_VersionID.m_Branch))
		{
			o_Error = "Branch is not valid";
			return false;
		}
		return true;
	}
	
	bool CVersionManager::fs_IsValidVersionIdentifier(CStr const &_String, CStr &o_Error, CVersionIdentifier *o_pVersionID)
	{
		CVersionIdentifier VersionID;
		aint iBranchEnd = _String.f_FindCharReverse('/');
		if (iBranchEnd < 0)
		{
			o_Error = "Branch was not found";
			return false;
		}
		VersionID.m_Branch = _String.f_Left(iBranchEnd);
		CStr VersionString = _String.f_Extract(iBranchEnd + 1);
		aint nParsed = 0;
		aint nChars = (CStr::CParse("{}.{}.{}") >> VersionID.m_Major >> VersionID.m_Minor >> VersionID.m_Revision).f_Parse(VersionString, nParsed);
		if (nParsed != 3)
		{
			o_Error = "Did not find all three version parts (Major, Minor and Revision)";
			return false;
		}
		if (nChars != VersionString.f_GetLen())
		{
			o_Error = "Extra characters after version";
			return false;
		}
		if (VersionID.m_Branch.f_IsEmpty())
		{
			o_Error = "Branch is empty";
			return false;
		}
		if (!fs_IsValidVersionIdentifier(VersionID, o_Error))
			return false;
		if (o_pVersionID)
			*o_pVersionID = VersionID;
		return true;
	}	
	
	// CVersionIdentifier
	
	void CVersionManager::CVersionIdentifier::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_Branch;
		_Stream << m_Major;
		_Stream << m_Minor;
		_Stream << m_Revision;
	}
	
	void CVersionManager::CVersionIdentifier::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Branch;
		_Stream >> m_Major;
		_Stream >> m_Minor;
		_Stream >> m_Revision;
	}

	CStr CVersionManager::CVersionIdentifier::f_EncodeFileName() const
	{
		return fg_Format("{}_{}.{}.{}", m_Branch.f_ReplaceChar('/', '_'), m_Major, m_Minor, m_Revision);
	}
	
	CStr CVersionManager::CVersionIdentifier::fs_DecodeFileName(CStr const &_FileName)  
	{
		return _FileName.f_ReplaceChar('_', '/');
	}
	
	void CVersionManager::CVersionIdentifier::f_Format(CStrAggregate &o_Str) const
	{
		o_Str += CStr::CFormat("{}/{}.{}.{}")
			<< m_Branch
			<< m_Major
			<< m_Minor
			<< m_Revision
		;
	}
	
	NEncoding::CEJSON CVersionManager::CVersionIdentifier::f_ToJSON() const
	{
		NEncoding::CEJSON Ret;
		Ret["Branch"] = m_Branch;
		Ret["Major"] = m_Major;
		Ret["Minor"] = m_Minor;
		Ret["Revision"] = m_Revision;
		return Ret;
	}
	
	bool CVersionManager::CVersionIdentifier::f_IsValid() const
	{
		return !m_Branch.f_IsEmpty();
	}

	auto CVersionManager::CVersionIdentifier::fs_FromJSON(NEncoding::CEJSON const &_JSON) -> CVersionIdentifier
	{
		CVersionIdentifier Ret;
		Ret.m_Branch = _JSON["Branch"].f_String();
		Ret.m_Major = _JSON["Major"].f_Integer();
		Ret.m_Minor = _JSON["Minor"].f_Integer();
		Ret.m_Revision = _JSON["Revision"].f_Integer();
		return Ret;
	}
	
	bool CVersionManager::CVersionIdentifier::operator == (CVersionIdentifier const &_Right) const
	{
		return NContainer::fg_TupleReferences(m_Branch, m_Major, m_Minor, m_Revision) 
			== NContainer::fg_TupleReferences(_Right.m_Branch, _Right.m_Major, _Right.m_Minor, _Right.m_Revision)
		;
	}
	
	bool CVersionManager::CVersionIdentifier::operator < (CVersionIdentifier const &_Right) const
	{
		return NContainer::fg_TupleReferences(m_Branch, m_Major, m_Minor, m_Revision) 
			< NContainer::fg_TupleReferences(_Right.m_Branch, _Right.m_Major, _Right.m_Minor, _Right.m_Revision)
		;
	}
	
	// CVersionInformation

	NEncoding::CEJSON CVersionManager::CVersionInformation::f_ToJSON() const
	{
		NEncoding::CEJSON Ret;
		Ret["Time"] = m_Time;
		Ret["Configuration"] = m_Configuration;
		auto &TagArray = Ret["Tags"].f_Array();
		for (auto &Tag : m_Tags)
			TagArray.f_Insert(Tag);
		Ret["ExtraInfo"] = m_ExtraInfo;
		Ret["NumFiles"] = m_nFiles;
		Ret["NumBytes"] = m_nBytes;
		return Ret;
	}
	
	auto CVersionManager::CVersionInformation::fs_FromJSON(NEncoding::CEJSON const &_JSON) -> CVersionInformation
	{
		CVersionInformation Ret;
		Ret.m_Time = _JSON["Time"].f_Date();
		Ret.m_Configuration = _JSON["Configuration"].f_String();
		if (auto *pValue = _JSON.f_GetMember("Tags", NEncoding::EJSONType_Array))
		{
			for (auto &Tag : pValue->f_Array())
				Ret.m_Tags[Tag.f_String()];
		}
		Ret.m_ExtraInfo = _JSON["ExtraInfo"];
		Ret.m_nFiles = _JSON["NumFiles"].f_Integer();
		Ret.m_nBytes = _JSON["NumBytes"].f_Integer();
		return Ret;
	}
	
	void CVersionManager::CVersionInformation::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_Time;
		_Stream << m_Configuration;
		_Stream << m_Tags;
		_Stream << m_ExtraInfo;
		_Stream << m_nFiles;
		_Stream << m_nBytes;
	}
	
	void CVersionManager::CVersionInformation::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Time;
		_Stream >> m_Configuration;
		_Stream >> m_Tags;
		_Stream >> m_ExtraInfo;
		_Stream >> m_nFiles;
		_Stream >> m_nBytes;
	}
	
	// CListApplications
	
	void CVersionManager::CListApplications::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_Applications;
	}
	
	void CVersionManager::CListApplications::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_Applications;
	}
	
	void CVersionManager::CListApplications::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}
	
	void CVersionManager::CListApplications::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}

	// CListVersions
	
	void CVersionManager::CListVersions::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_Versions;
	}
	
	void CVersionManager::CListVersions::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_Versions;
	}
	
	void CVersionManager::CListVersions::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_ForApplication;
	}
	
	void CVersionManager::CListVersions::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_ForApplication;
	}

	// CStartUploadTransfer
	
	void CVersionManager::CStartUploadTransfer::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		NConcurrency::fg_DistributedActorReturnFeed(_Stream, m_Subscription);
	}
	
	void CVersionManager::CStartUploadTransfer::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		NConcurrency::fg_DistributedActorReturnConsume(_Stream, m_Subscription);
	}

	void CVersionManager::CStartUploadTransfer::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		NConcurrency::fg_DistributedActorParamsFeed(_Stream, m_TransferContext);
	}
	
	void CVersionManager::CStartUploadTransfer::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		NConcurrency::fg_DistributedActorParamsConsume(_Stream, m_TransferContext);
	}

	// CStartUploadVersion
	
	void CVersionManager::CStartUploadVersion::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		NConcurrency::fg_DistributedActorReturnFeed(_Stream, m_Subscription);
		_Stream << m_DeniedTags;
	}
	
	void CVersionManager::CStartUploadVersion::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		NConcurrency::fg_DistributedActorReturnConsume(_Stream, m_Subscription);
		_Stream >> m_DeniedTags;
	}
		
	void CVersionManager::CStartUploadVersion::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_Application; 
		_Stream << m_VersionID; 
		_Stream << m_VersionInfo;
		_Stream << m_QueueSize;
		_Stream << m_Flags;
		NConcurrency::fg_DistributedActorParamsFeed(_Stream, m_DispatchActor);
		NConcurrency::fg_DistributedActorParamsFeed(_Stream, m_fStartTransfer);
	}
	
	void CVersionManager::CStartUploadVersion::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_Application; 
		_Stream >> m_VersionID; 
		_Stream >> m_VersionInfo; 
		_Stream >> m_QueueSize;
		_Stream >> m_Flags;
		NConcurrency::fg_DistributedActorParamsConsume(_Stream, m_DispatchActor);
		NConcurrency::fg_DistributedActorParamsConsume(_Stream, m_fStartTransfer);
	}
	
	void CVersionManager::CStartDownloadVersion::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		NConcurrency::fg_DistributedActorReturnFeed(_Stream, m_Subscription);
		_Stream << m_VersionInfo;
	}
	
	void CVersionManager::CStartDownloadVersion::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		NConcurrency::fg_DistributedActorReturnConsume(_Stream, m_Subscription);
		_Stream >> m_VersionInfo;
	}
		
	void CVersionManager::CStartDownloadVersion::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream << m_Application;
		_Stream << m_VersionID;
		_Stream << m_TransferContext;
	}
	
	void CVersionManager::CStartDownloadVersion::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream >> m_Application;
		_Stream >> m_VersionID;
		_Stream >> m_TransferContext;
	}
	
	// CNewVersionNotification
	
	void CVersionManager::CNewVersionNotifications::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
	}
	
	void CVersionManager::CNewVersionNotifications::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
	}
	
	void CVersionManager::CNewVersionNotification::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_Application;
		_Stream << m_VersionID;
		_Stream << m_VersionInfo;
	}
	
	void CVersionManager::CNewVersionNotification::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Application;
		_Stream >> m_VersionID;
		_Stream >> m_VersionInfo;
	}

	void CVersionManager::CNewVersionNotifications::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_bFullResend;
		_Stream << m_NewVersions;
	}
	
	void CVersionManager::CNewVersionNotifications::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_bFullResend;
		_Stream >> m_NewVersions;
	}

	// CSubscribeToUpdates
	
	void CVersionManager::CSubscribeToUpdates::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		NConcurrency::fg_DistributedActorReturnFeed(_Stream, m_Subscription);
	}
	
	void CVersionManager::CSubscribeToUpdates::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		NConcurrency::fg_DistributedActorReturnConsume(_Stream, m_Subscription);
	}

	void CVersionManager::CSubscribeToUpdates::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_Application; 
		NConcurrency::fg_DistributedActorParamsFeed(_Stream, m_DispatchActor);
		NConcurrency::fg_DistributedActorParamsFeed(_Stream, m_fOnNewVersions);
		_Stream << m_nInitial;
	}
	
	void CVersionManager::CSubscribeToUpdates::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Application; 
		NConcurrency::fg_DistributedActorParamsConsume(_Stream, m_DispatchActor);
		NConcurrency::fg_DistributedActorParamsConsume(_Stream, m_fOnNewVersions);
		_Stream >> m_nInitial;
	}
	
	// CChangeTags
	
	void CVersionManager::CChangeTags::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_DeniedTags;
	}
	
	void CVersionManager::CChangeTags::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_DeniedTags;
	}
	
	void CVersionManager::CChangeTags::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_Application;
		_Stream << m_VersionID;
		_Stream << m_AddTags;
		_Stream << m_RemoveTags;
	}
	
	void CVersionManager::CChangeTags::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Application;
		_Stream >> m_VersionID;
		_Stream >> m_AddTags;
		_Stream >> m_RemoveTags;
	}
}
