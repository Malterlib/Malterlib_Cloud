// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_LogAggregatorApp.h"
#include "Malterlib_Cloud_App_LogAggregator.h"

namespace NMib::NCloud::NLogAggregator
{
	CLogAggregatorApp::CLogAggregatorApp()
		: CDistributedAppActor(CDistributedAppActor_Settings{"LogAggregator", false}.f_AuditCategory("Malterlib/Cloud/LogAggregator"))
	{
	}
	
	CLogAggregatorApp::~CLogAggregatorApp()
	{
	}

	TCContinuation<void> CLogAggregatorApp::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		TCContinuation<void> Continuation;
		mp_Server = fg_ConstructActor<CLogAggregatorServer>(fg_Construct(self), mp_State);
		mp_Server(&CLogAggregatorServer::f_Init) > Continuation; 
		return Continuation;
	}
	
	TCContinuation<void> CLogAggregatorApp::fp_StopApp()
	{	
		TCSharedPointer<CCanDestroyTracker> pCanDestroy = fg_Construct();
		
		if (mp_Server)
		{
			DMibLogWithCategory(Mib/Cloud/LogAggregator/Daemon, Info, "Shutting down");
			
			mp_Server->f_Destroy2() > [pCanDestroy](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
						DMibLogWithCategory(Mib/Cloud/LogAggregator/Daemon, Error, "Failed to shut down server: {}", _Result.f_GetExceptionStr());
				}
			;
			mp_Server.f_Clear();
		}
		
		return pCanDestroy->m_Continuation;
	}
}
