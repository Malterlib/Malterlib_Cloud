// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ActorSubscription>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	CAppManagerCoordinationInterface::CAppManagerCoordinationInterface()
	{
		DPublishActorFunction(CAppManagerCoordinationInterface::f_RemoveKnownHost); 
		DPublishActorFunction(CAppManagerCoordinationInterface::f_SubscribeToAppChanges);
	}
	
	CAppManagerCoordinationInterface::~CAppManagerCoordinationInterface()
	{
	}

	static_assert(CVersionManager::EMinProtocolVersion <= 0x105, "");
	
	template <typename tf_CStream>
	void CAppManagerCoordinationInterface::CVersionIDAndPlatform::f_Stream(tf_CStream &_Stream)
	{
		DMibBinaryStreamVersion(_Stream, 0x105);
		CVersionManager::CVersionIDAndPlatform::f_Stream(_Stream);
	}
	DMibDistributedStreamImplement(CAppManagerCoordinationInterface::CVersionIDAndPlatform);

	template <typename tf_CStream>
	void CAppManagerCoordinationInterface::CAppInfo::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Group;
		_Stream % m_VersionManagerApplication;
		_Stream % m_VersionID;
		_Stream % m_VersionTime;	
		_Stream % m_FailedVersionID;
		_Stream % m_FailedVersionTime;
		_Stream % m_FailedVersionRetrySequence;
		_Stream % m_UpdateStage;
		_Stream % m_WantUpdateStage;
		_Stream % m_UpdateType;
		_Stream % m_UpdateStartSequence;
	}
	DMibDistributedStreamImplement(CAppManagerCoordinationInterface::CAppInfo);
	
	template <typename tf_CStream>
	void CAppManagerCoordinationInterface::CAppChange_Remove::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Application;
	}
	DMibDistributedStreamImplement(CAppManagerCoordinationInterface::CAppChange_Remove);
	
	template <typename tf_CStream>
	void CAppManagerCoordinationInterface::CAppChange_Update::f_Stream(tf_CStream &_Stream)
	{
		CAppInfo::f_Stream(_Stream);
		_Stream % m_Application;
	}
 	DMibDistributedStreamImplement(CAppManagerCoordinationInterface::CAppChange_Update);
	
	
	template <typename tf_CStream>
	void CAppManagerCoordinationInterface::CAppChange_AddKnownHosts::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Group;
		_Stream % m_VersionManagerApplication;
		_Stream % m_KnownHosts;
	}
	DMibDistributedStreamImplement(CAppManagerCoordinationInterface::CAppChange_AddKnownHosts);
	
	TCContinuation<void> CAppManagerActor::fp_PublishCoordinationInterface()
	{
		return mp_AppManagerCoordinationInterface.f_Publish<CAppManagerCoordinationInterface>(mp_State.m_DistributionManager, this, CAppManagerCoordinationInterface::mc_pDefaultNamespace);
	}
}
