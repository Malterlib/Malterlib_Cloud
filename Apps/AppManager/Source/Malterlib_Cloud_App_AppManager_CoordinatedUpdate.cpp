// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ActorSubscription>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_RemoveKnownHost(CEJSON const &_Params)
	{
		auto Auditor = f_Auditor();
		CStr Group = _Params["Group"].f_String();
		CStr Application = _Params["Application"].f_String();
		CStr HostID = _Params["HostID"].f_String();
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		TCActorResultVector<void> Results;
		
		for (auto &AppManager : mp_RemoteAppManagerState)
		{
			 if (!AppManager.m_Actor)
				 continue;
			DCallActor(AppManager.m_Actor, CAppManagerCoordinationInterface::f_RemoveKnownHost, Group, Application, HostID) > Results.f_AddResult();
		}
		
		mp_AppManagerCoordinationInterface.m_pActor->f_RemoveKnownHost(Group, Application, HostID) > Results.f_AddResult();
		
		Results.f_GetResults() > Continuation % Auditor / [Continuation](TCVector<TCAsyncResult<void>> &&_Results)
			{
				if (!fg_CombineResults(Continuation, fg_Move(_Results)))
					return;
				Continuation.f_SetResult();
			}
		;
		return Continuation;
	}
}
