// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/LogError>

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

	TCFuture<void> CCloudClientAppActor::fp_Destroy()
	{
		co_await fp_DestroyAll();

		co_await CDistributedAppActor::fp_Destroy();

		co_return {};
	}

	TCFuture<void> CCloudClientAppActor::fp_StopApp()
	{
		co_await fp_DestroyAll();

		co_return {};
	}

	TCFuture<void> CCloudClientAppActor::fp_DestroyAll()
	{
		TCFutureVector<void> Destroys;

		for (auto &StopPromise : mp_AppStopPromises)
			StopPromise.f_SetResult();

		if (mp_DownloadBackupSubscription)
			fg_Exchange(mp_DownloadBackupSubscription, nullptr)->f_Destroy() > Destroys;

		mp_BackupManagers.f_Destroy() > Destroys;

		mp_VersionManagerHelper.f_AbortAll() > Destroys;
		mp_VersionManagers.f_Destroy() > Destroys;

		mp_SecretsManagers.f_Destroy() > Destroys;

		if (mp_UploadSubscription)
			fg_Exchange(mp_UploadSubscription, nullptr)->f_Destroy() > Destroys;

		for (auto &Subscription : mp_TunnelSubscriptions)
			Subscription->f_Destroy() > Destroys;

		if (mp_TunnelsClient)
			fg_Move(mp_TunnelsClient).f_Destroy() > Destroys;

		mp_CloudManagers.f_Destroy() > Destroys;
		mp_CodeSigningManagers.f_Destroy() > Destroys;

		mp_DebugManagerHelper.f_AbortAll() > Destroys;
		mp_DebugManagers.f_Destroy() > Destroys;

		(co_await fg_AllDone(Destroys).f_Wrap()) > fg_LogError("Destroy", "Failed to destroy dependencies");

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
