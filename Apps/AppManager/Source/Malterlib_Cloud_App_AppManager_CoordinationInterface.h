// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Cloud/VersionManager>
#include <Mib/Cloud/AppManager>

namespace NMib::NCloud::NAppManager
{
	struct CAppManagerCoordinationInterface : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/AppManagerCoordination";
		
		CAppManagerCoordinationInterface();
		~CAppManagerCoordinationInterface();
		
		enum 
		{
			EMinProtocolVersion = 0x101
			, EProtocolVersion = 0x101
		};
		
		enum EAppChange
		{
			EAppChange_Update
			, EAppChange_Remove
			, EAppChange_AddKnownHosts
		};
		
		using EUpdateStage = CAppManagerInterface::EUpdateStage;

		struct CVersionIDAndPlatform : public CVersionManager::CVersionIDAndPlatform
		{
 			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			using CVersionManager::CVersionIDAndPlatform::CVersionIDAndPlatform;
			using CVersionManager::CVersionIDAndPlatform::operator =;
		};
		
		struct CAppInfo
		{
 			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CStr m_Group;
			CStr m_VersionManagerApplication;
			CVersionIDAndPlatform m_VersionID;
			CVersionIDAndPlatform m_WantVersionID;
			EUpdateStage m_UpdateStage = CAppManagerInterface::EUpdateStage_None;
			EUpdateStage m_WantUpdateStage = CAppManagerInterface::EUpdateStage_None;
			EDistributedAppUpdateType m_UpdateType = EDistributedAppUpdateType_Independent;
		};
		
		struct CAppChange_Update : public CAppInfo
		{
 			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			using CAppInfo::operator =;
			
			CStr m_Application;
		};
		
		struct CAppChange_Remove
		{
 			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			CStr m_Application;
		};

		struct CAppChange_AddKnownHosts
		{
 			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CStr m_Group;
			CStr m_VersionManagerApplication;
			TCSet<CStr> m_KnownHosts;
		};
		
		using CAppChange = TCStreamableVariant
			<
				EAppChange
				, CAppChange_Update, EAppChange_Update
				, CAppChange_Remove, EAppChange_Remove
				, CAppChange_AddKnownHosts, EAppChange_AddKnownHosts
			>
		;
		
		virtual TCContinuation<void> f_RemoveKnownHost(CStr const &_Group, CStr const &_Application, CStr const &_HostID) = 0; 
		virtual auto f_SubscribeToAppChanges(TCActorFunctorWithID<TCContinuation<void> (TCVector<CAppChange> const &_Changes, bool _bInitial)> &&_fOnChange) 
			-> TCContinuation<TCActorSubscriptionWithID<>> = 0 
		;
	};
}
