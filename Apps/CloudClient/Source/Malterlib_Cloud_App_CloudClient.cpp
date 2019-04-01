// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	CCloudClientAppActor::CCloudClientAppActor()
		: CDistributedAppActor{CDistributedAppActor_Settings{"MalterlibCloud"}}
	{
	}
	
	CCloudClientAppActor::~CCloudClientAppActor()
	{
	}

	TCFuture<void> CCloudClientAppActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		TCPromise<void> Promise;
		
		fp_ParseCommonOptions(_Params);
		
		Promise.f_SetResult();
		return Promise.f_MoveFuture();				
	}
	
	TCFuture<void> CCloudClientAppActor::fp_StopApp()
	{	
		TCActorResultVector<void> Destroys;

		for (auto &StopPromise : mp_AppStopPromises)
			StopPromise.f_SetResult();

		if (mp_DownloadBackupSubscription)
			mp_DownloadBackupSubscription->f_Destroy() > Destroys.f_AddResult();
		
		mp_VersionManagerHelper.f_AbortAll() > Destroys.f_AddResult();

		for (auto &Launch : mp_LaunchActors)
			Launch->f_Destroy() > Destroys.f_AddResult();

		TCPromise<void> Promise;
		Destroys.f_GetResults() > Promise.f_ReceiveAny();
		return Promise.f_MoveFuture();
	}
	
	void CCloudClientAppActor::fp_ParseCommonOptions(NEncoding::CEJSON const &_Params)
	{
		mp_Timeout = _Params["Timeout"].f_Float();
	}

}
namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_CloudClient()
	{
		return fg_Construct<NCloudClient::CCloudClientAppActor>();
	}
}
