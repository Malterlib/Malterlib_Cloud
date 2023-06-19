// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	CCloudClientAppActor::CCloudClientAppActor()
		: CDistributedAppActor
		{
			CDistributedAppActor_Settings{"MalterlibCloud"}
			.f_DefaultCommandLineFunctionalies(EDefaultCommandLineFunctionality_All & ~EDefaultCommandLineFunctionality_Sensor)
		}
		, mp_VersionManagerHelper(mp_State.m_RootDirectory)
	{
	}
	
	CCloudClientAppActor::~CCloudClientAppActor()
	{
	}

	TCFuture<void> CCloudClientAppActor::fp_StartApp(NEncoding::CEJSONSorted const &_Params)
	{
		fp_ParseCommonOptions(_Params);

		co_return {};
	}
	
	TCFuture<void> CCloudClientAppActor::fp_StopApp()
	{	
		TCActorResultVector<void> Destroys;

		for (auto &StopPromise : mp_AppStopPromises)
			StopPromise.f_SetResult();

		if (mp_DownloadBackupSubscription)
			mp_DownloadBackupSubscription->f_Destroy() > Destroys.f_AddResult();

		mp_BackupManagers.f_Destroy() > Destroys.f_AddResult();
		
		mp_VersionManagerHelper.f_AbortAll() > Destroys.f_AddResult();
		mp_VersionManagers.f_Destroy() > Destroys.f_AddResult();

		for (auto &Launch : mp_LaunchActors)
			Launch.f_Destroy() > Destroys.f_AddResult();

		mp_SecretsManagers.f_Destroy() > Destroys.f_AddResult();
		
		if (mp_UploadSubscription)
			mp_UploadSubscription->f_Destroy() > Destroys.f_AddResult();

		for (auto &Subscription : mp_TunnelSubscriptions)
			Subscription->f_Destroy() > Destroys.f_AddResult();

		if (mp_TunnelsClient)
			mp_TunnelsClient.f_Destroy() > Destroys.f_AddResult();

		mp_CloudManagers.f_Destroy() > Destroys.f_AddResult();

		co_await Destroys.f_GetResults();

		co_return {};
	}
	
	void CCloudClientAppActor::fp_ParseCommonOptions(NEncoding::CEJSONSorted const &_Params)
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
