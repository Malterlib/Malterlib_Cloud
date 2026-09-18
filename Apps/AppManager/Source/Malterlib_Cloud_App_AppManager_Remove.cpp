// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	NConcurrency::TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Remove(NStr::CStr _Name)
	{
		co_return co_await f_RemoveInternal(fg_Move(_Name), false);
	}

	NConcurrency::TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_RemoveInternal(NStr::CStr _Name, bool _bForce)
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

		if (CStr Environment = pThis->fp_FindEnvironmentStoringDataInApplication(_Name); !Environment.f_IsEmpty())
			co_return Auditor.f_Exception(fg_Format("Cannot remove application '{}' because environment '{}' stores its data inside it", _Name, Environment));

		CActorSubscription InProgressScope;
		if (!_bForce)
			InProgressScope = co_await (pThis->fp_SetInProgressWithWait(*pApplication, "Remove") % Auditor);
		else
		{
			// A forced remove does not wait for a stuck operation; pending launches
			// are aborted here and the operation unwinds against the deleted
			// application, which every launch step checks for after suspensions.
			// File work an update has already dispatched cannot be cancelled and can
			// still write to the directory, which an add with the same name reuses
			(*pApplication)->m_bPreventLaunch_User = true;
			(*pApplication)->f_AbortPendingLaunches();
		}

		auto DestroyInProgress = co_await fg_AsyncDestroy(fg_Move(InProgressScope));

		// A concurrent forced remove and add can replace the entry while the application stops
		auto pApplicationPtr = *pApplication;

		auto StopFuture = (*pApplication)->f_Stop(EStopFlag_CloseEncryption);
		if (_bForce)
			StopFuture = fg_Move(StopFuture).f_Timeout(30.0, "Timed out stopping the application during a forced remove");

		auto StopResult = co_await fg_Move(StopFuture).f_Wrap();
		if (CStr StopError = pThis->fp_GetApplicationStopErrors(StopResult, _Name); !StopError.f_IsEmpty())
			Auditor.f_Warning(StopError);

		pApplication = pThis->mp_Applications.f_FindEqual(_Name);
		if (!pApplication || *pApplication != pApplicationPtr)
			co_return Auditor.f_Exception(fg_Format("Application '{}' was removed while it stopped", _Name));

		// An environment can have been added with the application as parent while it stopped
		if (CStr Environment = pThis->fp_FindEnvironmentStoringDataInApplication(_Name); !Environment.f_IsEmpty())
			co_return Auditor.f_Exception(fg_Format("Cannot remove application '{}' because environment '{}' stores its data inside it", _Name, Environment));

		// The process can still run on the files
		if (!StopResult && pThis->fp_ApplicationRemoteStorageEnvironment(*pApplicationPtr))
			co_return Auditor.f_Exception(fg_Format("Application '{}' did not stop, so it and its files in the environment storage are kept", _Name));

		// Storage an environment manages for the application is deleted through
		// its agent; left-over storage would block a later add with the same name
		{
			auto RemoveStorageResult = co_await pThis->fp_RemoveApplicationEnvironmentStorage(pApplicationPtr).f_Wrap();
			if (!RemoveStorageResult && !_bForce)
			{
				co_return Auditor.f_Exception
					(
						fg_Format
						(
							"Failed to remove the application storage in the environment, so application '{}' is kept for a retry: {}"
							, _Name
							, RemoveStorageResult.f_GetExceptionStr()
						)
					)
				;
			}
			else if (!RemoveStorageResult)
				Auditor.f_Warning(fg_Format("Failed to remove the application storage in the environment: {}", RemoveStorageResult.f_GetExceptionStr()));
		}

		// A concurrent forced remove, or a remove and add, can replace the entry while the storage is removed
		pApplication = pThis->mp_Applications.f_FindEqual(_Name);
		if (!pApplication || *pApplication != pApplicationPtr)
			co_return Auditor.f_Exception(fg_Format("Application '{}' was removed while its storage was removed", _Name));

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
		bool bForce = _Params["Force"].f_AsBoolean(false);

		if (!bForce)
			fp_ReportInProgress(_pCommandLine, ApplicationName);

		co_await mp_AppManagerInterface.m_Actor(&CAppManagerInterfaceImplementation::f_RemoveInternal, ApplicationName, bForce);

		co_return 0;
	}
}
