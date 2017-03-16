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
		
		enum : uint32
		{
			EMinProtocolVersion = 0x103
			, EProtocolVersion = 0x103
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

#ifdef DCompiler_MSVC
			CVersionIDAndPlatform() = default;
			CVersionIDAndPlatform(CVersionIDAndPlatform const &) = default;
			CVersionIDAndPlatform(CVersionIDAndPlatform &&) = default;
			CVersionIDAndPlatform &operator = (CVersionIDAndPlatform const &) = default;
			CVersionIDAndPlatform &operator = (CVersionIDAndPlatform &&) = default;

			CVersionIDAndPlatform(CVersionManager::CVersionIDAndPlatform const &_Other) : CVersionManager::CVersionIDAndPlatform(_Other) { }
			CVersionIDAndPlatform &operator = (CVersionManager::CVersionIDAndPlatform const &_Right) { static_cast<CVersionManager::CVersionIDAndPlatform &>(*this) = _Right; return *this; }
#else
			using CVersionManager::CVersionIDAndPlatform::CVersionIDAndPlatform;
			using CVersionManager::CVersionIDAndPlatform::operator =;
#endif

		};
		
		struct CAppInfo
		{
 			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CStr m_Group;
			CStr m_VersionManagerApplication;
			CVersionIDAndPlatform m_VersionID;
			NTime::CTime m_VersionTime;
			CVersionIDAndPlatform m_FailedVersionID;
			NTime::CTime m_FailedVersionTime;
			uint32 m_FailedVersionRetrySequence = 0;
			EUpdateStage m_UpdateStage = EUpdateStage::EUpdateStage_None;
			EUpdateStage m_WantUpdateStage = EUpdateStage::EUpdateStage_None;
			EDistributedAppUpdateType m_UpdateType = EDistributedAppUpdateType_Independent;
			uint64 m_UpdateStartSequence = TCLimitsInt<uint64>::mc_Max;
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
