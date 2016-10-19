// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_CloudAPIManager.h"

namespace NMib::NCloud
{
	using namespace NStr;
	
	bool CCloudAPIManager::fs_IsValidCloudContext(CStr const &_String)
	{
		return NNet::fg_IsValidHostname(_String);
	}
	
	bool CCloudAPIManager::fs_IsValidContainerName(NStr::CStr const &_String)
	{
		return NNet::fg_IsValidHostname(_String);
	}
	
	bool CCloudAPIManager::fs_IsValidTempURLKey(NStr::CStr const &_String)
	{
		if (_String.f_IsEmpty())
			return false;
		if (_String.f_GetLen() > 254)
			return false;
		ch8 const *pParse = _String.f_GetStr();
		while (*pParse)
		{
			while (*pParse && (NStr::fg_CharIsAnsiAlphabetical(*pParse) || NStr::fg_CharIsNumber(*pParse) || *pParse == '-' || *pParse == '_'))
				++pParse;
			
			if (*pParse)
				return false; // Any other character is not allowed
		}
		return true;
	}
	
	bool CCloudAPIManager::fs_IsValidObjectId(NStr::CStr const &_String)
	{
		return !_String.f_IsEmpty();
	}
	
	bool CCloudAPIManager::fs_IsValidMethod(NStr::CStr const &_String)
	{
		if (_String == "GET" || _String == "PUT" || _String == "DELETE")
			return true;

		return false;
	}

	
	// CEnsureContainer

	void CCloudAPIManager::CEnsureContainer::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
	}
	
	void CCloudAPIManager::CEnsureContainer::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
	}
	
	void CCloudAPIManager::CEnsureContainer::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_CloudContext;
		_Stream << m_ContainerName;
		_Stream << m_TempURLKey; 
	}
	
	void CCloudAPIManager::CEnsureContainer::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_CloudContext;
		_Stream >> m_ContainerName;
		_Stream >> m_TempURLKey; 
	}
	
	// CSignTempURL
	
	void CCloudAPIManager::CSignTempURL::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_SignedURL;
	}
	
	void CCloudAPIManager::CSignTempURL::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_SignedURL;
	}
	
	void CCloudAPIManager::CSignTempURL::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_CloudContext;
		_Stream << m_Method;
		_Stream << m_ContainerName;
		_Stream << m_ObjectId;
		_Stream << m_TempURLKey;
	}
	
	void CCloudAPIManager::CSignTempURL::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_CloudContext;
		_Stream >> m_Method;
		_Stream >> m_ContainerName;
		_Stream >> m_ObjectId;
		_Stream >> m_TempURLKey;
	}
	
	// CDeleteObject

	void CCloudAPIManager::CDeleteObject::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
	}
	
	void CCloudAPIManager::CDeleteObject::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
	}
	
	void CCloudAPIManager::CDeleteObject::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_CloudContext;
		_Stream << m_ContainerName;
		_Stream << m_ObjectId;
	}
	
	void CCloudAPIManager::CDeleteObject::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_CloudContext;
		_Stream >> m_ContainerName;
		_Stream >> m_ObjectId;
	}
}
