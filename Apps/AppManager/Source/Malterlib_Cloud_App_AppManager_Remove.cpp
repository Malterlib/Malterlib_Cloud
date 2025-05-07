// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	NConcurrency::TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Remove(NStr::CStr _Name)
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationRemove"}};
		Permissions["App"] = {CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)};

		auto HasPermissions = co_await (pThis->mp_Permissions.f_HasPermissions("Remove application from AppManager", Permissions) % "Permission denied removing application" % Auditor);

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Application remove, command)", Permissions["Command"]);

		if (!HasPermissions["App"])
			co_return Auditor.f_AccessDenied("(Application remove, app name)", Permissions["App"]);

		auto *pApplication = pThis->mp_Applications.f_FindEqual(_Name);
		if (!pApplication)
			co_return Auditor.f_Exception(fg_Format("No such application '{}'", _Name));

		auto InProgressScope = co_await (pThis->fp_SetInProgressWithWait(*pApplication, "Remove") % Auditor);
		auto DestroyInProgress = co_await fg_AsyncDestroy(fg_Move(InProgressScope));
		
		if (CStr Error = pThis->fp_GetApplicationStopErrors(co_await (*pApplication)->f_Stop(EStopFlag_CloseEncryption).f_Wrap(), _Name); !Error.f_IsEmpty())
			Auditor.f_Warning(Error);

		pApplication = pThis->mp_Applications.f_FindEqual(_Name);
		if (!pApplication)
			co_return Auditor.f_Exception(fg_Format("No such application '{}'", _Name));

		auto pApplicationPtr = *pApplication;
		(*pApplication)->f_Delete();
		pThis->mp_Applications.f_Remove(_Name);
		pThis->fp_SendRemovedAppToRemoteAppManagers(pApplicationPtr);
		pThis->fp_SendAppChange_Removed(*pApplicationPtr);
		pThis->fp_UpdateLimits();

		if (auto *pApplicationsState = pThis->mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
		{
			if (pApplicationsState->f_GetMember(_Name))
				pApplicationsState->f_RemoveMember(_Name);
		}

		co_await pThis->fp_RebootPrevention_RemoveApplication(_Name);
		co_await (pThis->mp_State.m_StateDatabase.f_Save() % "Failed to save state" % Auditor);

		Auditor.f_Info(fg_Format("Removed application '{}'", _Name));

		co_await fg_Move(DestroyInProgress);
		co_await pThis->fp_SyncNotifications(_Name);

		co_return {};
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_RemoveApplication(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr ApplicationName = _Params["Name"].f_String();
		fp_ReportInProgress(_pCommandLine, ApplicationName);

		co_await mp_AppManagerInterface.m_Actor(&CAppManagerInterfaceImplementation::f_Remove, _Params["Name"].f_String());

		co_return 0;
	}
}
