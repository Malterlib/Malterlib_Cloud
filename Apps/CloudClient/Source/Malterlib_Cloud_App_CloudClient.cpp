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
		, mp_DebugManagerHelper(mp_State.m_RootDirectory)
	{
	}
	
	CCloudClientAppActor::~CCloudClientAppActor()
	{
	}

	TCFuture<void> CCloudClientAppActor::fp_StartApp(NEncoding::CEJsonSorted const _Params)
	{
		fp_ParseCommonOptions(_Params);

		co_return {};
	}
	
	TCFuture<void> CCloudClientAppActor::fp_StopApp()
	{	
		TCFutureVector<void> Destroys;

		for (auto &StopPromise : mp_AppStopPromises)
			StopPromise.f_SetResult();

		if (mp_DownloadBackupSubscription)
			mp_DownloadBackupSubscription->f_Destroy() > Destroys;

		mp_BackupManagers.f_Destroy() > Destroys;
		
		mp_VersionManagerHelper.f_AbortAll() > Destroys;
		mp_VersionManagers.f_Destroy() > Destroys;

		mp_SecretsManagers.f_Destroy() > Destroys;
		
		if (mp_UploadSubscription)
			mp_UploadSubscription->f_Destroy() > Destroys;

		for (auto &Subscription : mp_TunnelSubscriptions)
			Subscription->f_Destroy() > Destroys;

		if (mp_TunnelsClient)
			fg_Move(mp_TunnelsClient).f_Destroy() > Destroys;

		mp_CloudManagers.f_Destroy() > Destroys;

		mp_DebugManagerHelper.f_AbortAll() > Destroys;
		mp_DebugManagers.f_Destroy() > Destroys;

		co_await fg_AllDoneWrapped(Destroys);

		co_return {};
	}
	
	void CCloudClientAppActor::fp_ParseCommonOptions(NEncoding::CEJsonSorted const &_Params)
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
