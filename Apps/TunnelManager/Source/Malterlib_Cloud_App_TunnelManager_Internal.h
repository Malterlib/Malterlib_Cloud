// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedAppSensorStoreLocal>
#include <Mib/Concurrency/DistributedAppLogStoreLocal>
#include <Mib/File/ChangeNotificationActor>
//#include <Mib/Cloud/TunnelManager>
#include <Mib/Cloud/FileTransfer>
#include <Mib/Database/DatabaseActor>

#include "Malterlib_Cloud_App_TunnelManager.h"

namespace NMib::NCloud::NTunnelManager
{
	struct CTunnelManagerServer : public CActor
	{
	public:
		using CActorHolder = CDelegatedActorHolder;

		CTunnelManagerServer(CDistributedAppState &_AppState);
		~CTunnelManagerServer();

/*		struct CTunnelManagerImplementation : public CTunnelManager
		{
			TCFuture<TCActorSubscriptionWithID<>> f_RegisterAppManager(TCDistributedActorInterfaceWithID<CAppManagerInterface> &&_AppManager, CAppManagerInfo &&_AppManagerInfo) override;
			TCFuture<TCMap<CStr, CAppManagerDynamicInfo>> f_EnumAppManagers() override;
			TCFuture<TCMap<CApplicationKey, CApplicationInfo>> f_EnumApplications() override;
			TCFuture<void> f_RemoveAppManager(NStr::CStr const &_AppManagerHostID) override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppSensorReporter>> f_GetSensorReporter() override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>> f_GetSensorReader() override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppLogReporter>> f_GetLogReporter() override;
			TCFuture<TCDistributedActorInterfaceWithID<CDistributedAppLogReader>> f_GetLogReader() override;

			DMibDelegatedActorImplementation(CTunnelManagerServer);
		};*/

		TCFuture<void> f_Init();

	private:
		TCFuture<void> fp_Destroy() override;
		TCFuture<void> fp_Publish();
		TCFuture<void> fp_SetupPermissions();

		//TCDistributedActorInstance<CTunnelManagerImplementation> mp_ProtocolInterface;
		CDistributedAppState &mp_AppState;

		CTrustedPermissionSubscription mp_Permissions;
	};
}
