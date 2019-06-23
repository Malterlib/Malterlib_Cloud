// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ActorCallbackManager>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/File/ChangeNotificationActor>
#include <Mib/Cloud/CloudManager>
#include <Mib/Cloud/FileTransfer>
#include <Mib/Database/DatabaseActor>

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

namespace NMib::NCloud::NCloudManager
{
	struct CCloudManagerServer : public CActor
	{
	public:
		using CActorHolder = CDelegatedActorHolder;

		CCloudManagerServer(CDistributedAppState &_AppState);
		~CCloudManagerServer();

		struct CCloudManagerImplementation : public CCloudManager
		{
			TCFuture<TCActorSubscriptionWithID<>> f_RegisterAppManager(TCDistributedActorInterfaceWithID<CAppManagerInterface> &&_AppManager, CAppManagerInfo &&_AppManagerInfo) override;
			TCFuture<TCMap<CStr, CAppManagerDynamicInfo>> f_EnumAppManagers() override;

			CCloudManagerServer *m_pThis;
		};

		TCFuture<void> f_Init();

	private:
		struct CAppManagerState
		{
			CStr const &f_AppManagerID() const;
			NCloudManagerDatabase::CAppManagerKey f_DatabaseKey() const;

			TCDistributedActorInterfaceWithID<CAppManagerInterface> m_Interface;
			NCloudManagerDatabase::CAppManagerValue m_Data;
			CStr m_UniqueHostID;
			mint m_RegisterSequence = 0;
		};

		TCFuture<void> fp_Destroy() override;
		TCFuture<void> fp_Publish();
		TCFuture<void> fp_SetupPermissions();
		TCFuture<void> fp_SetupMonitor();
		TCFuture<void> fp_UpdateAppManagerState();
		TCFuture<void> fp_SaveAppManagerData(NCloudManagerDatabase::CAppManagerKey _Key, NCloudManagerDatabase::CAppManagerValue _Data);

		TCDistributedActorInstance<CCloudManagerImplementation> mp_ProtocolInterface;
		CDistributedAppState &mp_AppState;

		CTrustedPermissionSubscription mp_Permissions;

		TCActor<CDatabaseActor> mp_DatabaseActor;

		mint mp_AppManagerRegisterSequence = 0;
		TCMap<CStr, CAppManagerState> mp_AppManagers;

		CActorSubscription mp_MonitorTimerSubscription;
	};
}
