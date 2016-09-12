// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_VersionManager.h"

namespace NMib::NCloud
{
	using namespace NStr;
	
	uint32 CVersionManager::f_GetProtocolVersion(uint32 _Version)
	{
		return fg_Min(uint32(EProtocolVersion), _Version);
	}
	
	bool CVersionManager::fs_IsValidApplicationName(CStr const &_String)
	{
		return NNet::fg_IsValidHostname(_String);
	}
	
	bool CVersionManager::fs_IsValidProtocolVersion(uint32 _Version)
	{
		return _Version >= EMinProtocolVersion && _Version <= EProtocolVersion;
	}
	
	bool CVersionManager::fs_IsValidVersionIdentifier(CVersionIdentifier const &_VersionID, CStr &o_Error)
	{
		if (!NNet::fg_IsValidHostname(_VersionID.m_Branch, "/"))
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
	
	void CVersionManager::CVersionInformation::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_Time;
		_Stream << m_Configuration;
		_Stream << m_ExtraInfo;
	}
	
	void CVersionManager::CVersionInformation::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Time;
		_Stream >> m_Configuration;
		_Stream >> m_ExtraInfo;
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
	}
	
	void CVersionManager::CStartUploadVersion::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		NConcurrency::fg_DistributedActorReturnConsume(_Stream, m_Subscription);
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
	}
	
	void CVersionManager::CStartDownloadVersion::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		NConcurrency::fg_DistributedActorReturnConsume(_Stream, m_Subscription);
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
}
