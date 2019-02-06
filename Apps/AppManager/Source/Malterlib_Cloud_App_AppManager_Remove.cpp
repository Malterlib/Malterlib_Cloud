// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	NConcurrency::TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Remove(NStr::CStr const &_Name)
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationRemove"}};
		Permissions["App"] = {CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)};

		TCPromise<void> Promise;

		pThis->mp_Permissions.f_HasPermissions("Remove application from AppManager", Permissions)
			> Promise % "Permission denied removing application" % Auditor / [=](NContainer::TCMap<NStr::CStr, bool> const &_HasPermissions)
			{
				if (!_HasPermissions["Command"])
					return Promise.f_SetException(Auditor.f_AccessDenied("(Application remove, command)"));

				if (!_HasPermissions["App"])
					return Promise.f_SetException(Auditor.f_AccessDenied("(Application remove, app name)"));

				auto *pApplication = pThis->mp_Applications.f_FindEqual(_Name);
				if (!pApplication)
					return Promise.f_SetException(Auditor.f_Exception(fg_Format("No such application '{}'", _Name)));

				auto &Application = **pApplication;
				if (Application.f_IsInProgress())
					return Promise.f_SetException(Auditor.f_Exception("Operation already in progress for application"));
				auto InProgressScope = Application.f_SetInProgress();

				Application.f_Stop(EStopFlag_CloseEncryption) > [pThis, Auditor, Promise, _Name, InProgressScope](TCAsyncResult<uint32> &&_Result)
					{
						CStr Error = pThis->fp_GetApplicationStopErrors(_Result, _Name);

						if (!Error.f_IsEmpty())
							Auditor.f_Warning(Error);

						auto *pApplication = pThis->mp_Applications.f_FindEqual(_Name);
						if (!pApplication)
							return Promise.f_SetException(Auditor.f_Exception(fg_Format("No such application '{}'", _Name)));

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

						pThis->mp_State.m_StateDatabase.f_Save() > Promise % "Failed to save state" % Auditor / [InProgressScope, Promise, _Name, Auditor]() mutable
							{
								Auditor.f_Info(fg_Format("Removed application '{}'", _Name));
								Promise.f_SetResult();
							}
						;
					}
				;
			}
		;

		return Promise.f_MoveFuture();
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_RemoveApplication(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCPromise<uint32> Promise;
		
		mp_AppManagerInterface.m_pActor->f_Remove(_Params["Name"].f_String()) > [Promise](TCAsyncResult<void> &&_Result)
			{
				Promise.f_SetExceptionOrResult(_Result, 0);
			}
		;
		
		return Promise.f_MoveFuture();
	}
}
