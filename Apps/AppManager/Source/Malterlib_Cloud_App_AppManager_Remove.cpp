// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	NConcurrency::TCContinuation<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Remove(NStr::CStr const &_Name)
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissions>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationRemove"}};
		Permissions["App"] = {CPermissions{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)};

		TCContinuation<void> Continuation;

		pThis->mp_Permissions.f_HasPermissions("Remove application from AppManager", Permissions) > Continuation / [=](NContainer::TCMap<NStr::CStr, bool> const &_HasPermissions)
			{
				if (!_HasPermissions["Command"])
					return Continuation.f_SetException(Auditor.f_AccessDenied("(Application remove, command)"));

				if (!_HasPermissions["App"])
					return Continuation.f_SetException(Auditor.f_AccessDenied("(Application remove, app name)"));

				auto *pApplication = pThis->mp_Applications.f_FindEqual(_Name);
				if (!pApplication)
					return Continuation.f_SetException(Auditor.f_Exception(fg_Format("No such application '{}'", _Name)));

				auto &Application = **pApplication;
				if (Application.f_IsInProgress())
					return Continuation.f_SetException(Auditor.f_Exception("Operation already in progress for application"));
				auto InProgressScope = Application.f_SetInProgress();

				Application.f_Stop(EStopFlag_CloseEncryption) > [pThis, Auditor, Continuation, _Name, InProgressScope](TCAsyncResult<uint32> &&_Result)
					{
						CStr Error = pThis->fp_GetApplicationStopErrors(_Result, _Name);

						if (!Error.f_IsEmpty())
							Auditor.f_Warning(Error);

						auto *pApplication = pThis->mp_Applications.f_FindEqual(_Name);
						if (!pApplication)
							return Continuation.f_SetException(Auditor.f_Exception(fg_Format("No such application '{}'", _Name)));

						auto pApplicationPtr = *pApplication;
						(*pApplication)->f_Delete();
						pThis->mp_Applications.f_Remove(_Name);
						pThis->fp_SendRemovedAppToRemoteAppManagers(pApplicationPtr);
						pThis->fp_UpdateLimits();

						if (auto *pApplicationsState = pThis->mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
						{
							if (pApplicationsState->f_GetMember(_Name))
								pApplicationsState->f_RemoveMember(_Name);
						}

						pThis->mp_State.m_StateDatabase.f_Save() > Continuation % "Failed to save state" % Auditor / [InProgressScope, Continuation, _Name, Auditor]() mutable
							{
								Auditor.f_Info(fg_Format("Removed application '{}'", _Name));
								Continuation.f_SetResult();
							}
						;
					}
				;
			}
		;

		return Continuation;
	}

	TCContinuation<uint32> CAppManagerActor::fp_CommandLine_RemoveApplication(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCContinuation<uint32> Continuation;
		
		mp_AppManagerInterface.m_pActor->f_Remove(_Params["Name"].f_String()) > [Continuation](TCAsyncResult<void> &&_Result)
			{
				Continuation.f_SetExceptionOrResult(_Result, 0);
			}
		;
		
		return Continuation;
	}
}
