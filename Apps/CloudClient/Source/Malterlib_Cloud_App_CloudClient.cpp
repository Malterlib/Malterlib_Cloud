// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	CCloudClientAppActor::CCloudClientAppActor()
		: CDistributedAppActor{CDistributedAppActor_Settings{"MalterlibCloud", false}}
	{
	}
	
	CCloudClientAppActor::~CCloudClientAppActor()
	{
	}

	TCContinuation<void> CCloudClientAppActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		TCContinuation<void> Continuation;
		
		fp_ParseCommonOptions(_Params);
		
		Continuation.f_SetResult();
		return Continuation;				
	}
	
	TCContinuation<void> CCloudClientAppActor::fp_StopApp()
	{	
		TCActorResultVector<void> Destroys;

		mp_DownloadBackupSubscription.f_Clear();
		mp_VersionManagerHelper.f_AbortAll() > Destroys.f_AddResult();  
		
		TCContinuation<void> Continuation;
		Destroys.f_GetResults() > Continuation.f_ReceiveAny();
		return Continuation;
	}
	
	void CCloudClientAppActor::fp_ParseCommonOptions(NEncoding::CEJSON const &_Params)
	{
		mp_Timeout = _Params["Timeout"].f_Float();
	}
}
