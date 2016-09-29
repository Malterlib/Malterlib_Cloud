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
		return NNet::fg_IsValidHostname(_String);
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
}
