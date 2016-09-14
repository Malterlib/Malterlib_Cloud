// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_RemoveApplication(CEJSON const &_Params)
	{
		CStr Name = _Params["Name"].f_String();
		auto *pApplication = mp_Applications.f_FindEqual(Name);
		if (!pApplication)
			return DMibErrorInstance(fg_Format("No such application '{}'", Name));
		
		auto &Application = **pApplication;
		if (Application.m_bOperationInProgress)
			return DMibErrorInstance("Operation already in progress for application");
		auto InProgressScope = Application.f_SetInProgress();
			
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		Application.f_Stop(true) > [this, Continuation, Name, InProgressScope](TCAsyncResult<uint32> &&_Result)
			{
				CDistributedAppCommandLineResults Results;
				fp_OutputApplicationStop(_Result, Results, Name);
				
				auto *pApplication = mp_Applications.f_FindEqual(Name);
				if (!pApplication)
					return Continuation.f_SetException(DMibErrorInstance(fg_Format("No such application '{}'", Name)));
				
				(*pApplication)->f_Delete();
				mp_Applications.f_Remove(Name);

				if (auto *pApplicationsState = mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
				{
					if (pApplicationsState->f_GetMember(Name))
						pApplicationsState->f_RemoveMember(Name);
				}
				
				mp_State.m_StateDatabase.f_Save() > [Results = fg_Move(Results), Continuation, InProgressScope](TCAsyncResult<void> &&_Result) mutable
					{
						if (!_Result)
						{
							Results.f_AddStdErr(fg_Format("Failed to save state: {}{\n}", _Result.f_GetExceptionStr()));
							Results.m_Status = 1;
							Continuation.f_SetResult(fg_Move(Results));
							return;
						}
						
						Continuation.f_SetResult(fg_Move(Results));
					}
				;
			}
		;
		
		return Continuation;
	}
}
