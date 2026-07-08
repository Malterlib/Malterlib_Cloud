// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Atomic/Atomic>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/RandomID>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_StartEnvironmentVM(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		using namespace NVirtualization;

		auto &Settings = _pEnvironment->m_Settings;

		EVirtualizationBackend Backend = EVirtualizationBackend_Default;
		if (Settings.m_VMBackend == "MacOSVirtualization")
			Backend = EVirtualizationBackend_MacOSVirtualization;
		else if (!Settings.m_VMBackend.f_IsEmpty())
		{
			CStr Error = "Cannot start environment '{}': unknown VM backend '{}'"_f << _pEnvironment->m_Name << Settings.m_VMBackend;
			_pEnvironment->f_SetStatus(Error, CAppManagerInterface::EStatusSeverity_Error);
			co_return DMibErrorInstance(Error);
		}

		if (!fg_IsVirtualizationBackendAvailable(Backend))
		{
			CStr Error = "Cannot start environment '{}': no virtualization backend is available on this host"_f << _pEnvironment->m_Name;
			_pEnvironment->f_SetStatus(Error, CAppManagerInterface::EStatusSeverity_Error);
			co_return DMibErrorInstance(Error);
		}

		CStr VMImagesBaseDirectory = mp_State.m_RootDirectory;
		if (auto pParentApplication = fp_GetEnvironmentParentApplication(*_pEnvironment))
			VMImagesBaseDirectory = pParentApplication->f_GetDirectory();

		CStr BundleDirectory = fg_Format("{}/VMImages/{}", VMImagesBaseDirectory, Settings.m_VMImage);

		bool bBundleExists;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			bBundleExists = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [BundleDirectory]()
					{
						return CFile::fs_FileExists(BundleDirectory, EFileAttrib_Directory);
					}
				)
			;
		}

		if (!bBundleExists)
		{
			CStr Error = "Cannot start environment '{}': VM image bundle '{}' does not exist"_f << _pEnvironment->m_Name << BundleDirectory;
			_pEnvironment->f_SetStatus(Error, CAppManagerInterface::EStatusSeverity_Error);
			co_return DMibErrorInstance(Error);
		}

		_pEnvironment->m_bStarting = true;
		_pEnvironment->m_bStopping = false;
		_pEnvironment->f_SetStatus("Starting VM", CAppManagerInterface::EStatusSeverity_Warning);

		bool bConnected = false;
		auto Cleanup = g_OnScopeExit / [&, _pEnvironment]
			{
				_pEnvironment->m_bStarting = false;
				auto OnAgentConnected = fg_Move(_pEnvironment->m_OnAgentConnected);
				for (auto &Promise : OnAgentConnected)
				{
					if (bConnected)
						Promise.f_SetResult();
					else
						Promise.f_SetException(DMibErrorInstance("Environment '{}' failed to start"_f << _pEnvironment->m_Name));
				}
			}
		;

		_pEnvironment->m_LaunchID = fg_RandomID();

		CVirtualMachineConfig Config;
		Config.m_BundleDirectory = BundleDirectory;
		Config.m_CPUCount = Settings.m_VMCPUCount;
		Config.m_MemoryMB = Settings.m_VMMemoryMB;

		// Environments confined to a parent application only share their own storage with
		// the guest, so all guest-visible data stays inside the parent application directory
		if (_pEnvironment->m_Settings.m_ParentApplication.f_IsEmpty())
			Config.m_SharedFolders["MalterlibRoot"] = mp_State.m_RootDirectory;
		else
			Config.m_SharedFolders["MalterlibRoot"] = fp_GetEnvironmentStorageDirectory(*_pEnvironment);

		_pEnvironment->m_VMActor = fg_CreateVirtualMachine(Backend, fg_Move(Config));

		auto StartResult = co_await _pEnvironment->m_VMActor(&CVirtualMachineActor::f_Start).f_Wrap();

		if (!StartResult)
		{
			_pEnvironment->f_SetStatus(fg_Format("Failed to start VM: {}", StartResult.f_GetExceptionStr()), CAppManagerInterface::EStatusSeverity_Error);
			fp_StopEnvironmentInternal(_pEnvironment).f_DiscardResult();
			co_return StartResult.f_GetException();
		}

		_pEnvironment->f_SetStatus("VM running, waiting for agent", CAppManagerInterface::EStatusSeverity_Warning);

		// The guest image is expected to run an installed agent that connects back
		// to this AppManager and registers its environment interface
		if (!_pEnvironment->f_IsStarted())
		{
			auto ConnectedResult = co_await _pEnvironment->m_OnAgentConnected.f_Insert().f_Future()
				.f_Timeout(300.0, "Timed out waiting for the environment agent in the VM to connect")
				.f_Wrap()
			;

			if (!ConnectedResult)
			{
				_pEnvironment->f_SetStatus(fg_Format("Agent failed to connect: {}", ConnectedResult.f_GetExceptionStr()), CAppManagerInterface::EStatusSeverity_Error);
				fp_StopEnvironmentInternal(_pEnvironment).f_DiscardResult();
				co_return ConnectedResult.f_GetException();
			}
		}

		bConnected = true;

		co_return {};
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_CreateVMImage(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		using namespace NVirtualization;

		CStr Name = _Params["Name"].f_String();
		CStr RestoreImage = _Params["RestoreImage"].f_String();

		if (!fg_IsVirtualizationBackendAvailable(EVirtualizationBackend_Default))
		{
			co_await _pCommandLine->f_StdErr("No virtualization backend is available on this host\n");
			co_return 1;
		}

		CStr BaseDirectory = mp_State.m_RootDirectory;

		CStr ParentApplication = _Params["ParentApplication"].f_String();
		if (!ParentApplication.f_IsEmpty())
		{
			auto *pFindApplication = mp_Applications.f_FindEqual(ParentApplication);
			if (!pFindApplication)
			{
				co_await _pCommandLine->f_StdErr("Parent application '{}' does not exist\n"_f << ParentApplication);
				co_return 1;
			}

			if ((*pFindApplication)->f_NeedsEncryption() && !(*pFindApplication)->f_EncryptionOpened())
			{
				co_await _pCommandLine->f_StdErr("Parent application '{}' encryption is not yet opened\n"_f << ParentApplication);
				co_return 1;
			}

			BaseDirectory = (*pFindApplication)->f_GetDirectory();
		}

		CStr BundleDirectory = fg_Format("{}/VMImages/{}", BaseDirectory, Name);

		bool bBundleExists;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			bBundleExists = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [BundleDirectory]()
					{
						return CFile::fs_FileExists(BundleDirectory, EFileAttrib_Directory);
					}
				)
			;
		}

		if (bBundleExists)
		{
			co_await _pCommandLine->f_StdErr("The VM image '{}' already exists\n"_f << Name);
			co_return 1;
		}

		CMacOSVMImageCreateParams CreateParams;
		CreateParams.m_BundleDirectory = BundleDirectory;
		CreateParams.m_RestoreImagePath = RestoreImage;
		CreateParams.m_CPUCount = (uint32)_Params["VMCPUCount"].f_AsInteger();
		CreateParams.m_MemoryMB = (uint32)_Params["VMMemoryMB"].f_AsInteger();
		CreateParams.m_DiskSizeGB = (uint64)_Params["DiskSizeGB"].f_AsInteger();

		co_await _pCommandLine->f_StdOut("Creating VM image '{}' and installing macOS from {}\n"_f << Name << RestoreImage);

		NStorage::TCSharedPointer<NAtomic::TCAtomic<fp64>> pProgress = fg_Construct(0.0);
		NStorage::TCSharedPointer<NAtomic::TCAtomic<bool>> pDone = fg_Construct(false);

		TCPromiseFuturePair<void> DonePromise;
		{
			auto Promise = DonePromise.m_Promise;
			fg_CreateMacOSVMImage
				(
					fg_Move(CreateParams)
					, [pProgress](fp64 _Progress)
					{
						*pProgress = _Progress;
					}
				)
				> [Promise, pDone](TCAsyncResult<void> _Result) mutable
				{
					*pDone = true;
					if (!_Result)
						Promise.f_SetException(_Result.f_GetException());
					else
						Promise.f_SetResult();
				}
			;
		}

		fp64 LastReportedProgress = -1.0;
		while (!*pDone)
		{
			co_await fg_Timeout(2.0);

			fp64 Progress = *pProgress;
			if (Progress > LastReportedProgress)
			{
				LastReportedProgress = Progress;
				co_await _pCommandLine->f_StdOut("Installing: {}%\n"_f << aint((Progress * 100.0).f_Get()));
			}
		}

		auto Result = co_await fg_Move(DonePromise.m_Future).f_Wrap();

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}
}
