// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Concurrency/DistributedDaemon>

#ifdef DPlatformFamily_macOS
#include <Mib/Virtualization/VirtualMachine>
#endif

#ifdef DPlatformFamily_Windows
#include <Windows.h>
#endif

#include "Malterlib_Cloud_App_AppManager.h"

using namespace NMib;
using namespace NMib::NCloud::NAppManager;

#ifdef DPlatformFamily_macOS
namespace
{
	NConcurrency::TCFuture<NEncoding::CEJsonSorted> fg_VirtualMachineWindowAction
		(
			NEncoding::CEJsonSorted _Params
			, NStorage::TCSharedPointer<NConcurrency::CRunLoop> _pRunLoop
		)
	{
		using namespace NVirtualization;

		struct CWindowRun
		{
			CVirtualMachineConfig m_Config;
			EVirtualizationBackend m_Backend = EVirtualizationBackend_Default;
			NStr::CStr m_Title;
			NConcurrency::TCPromise<void> m_Promise;
		};

		NConcurrency::TCPromiseFuturePair<void> Done;

		NStorage::TCSharedPointer<CWindowRun> pRun = fg_Construct();
		pRun->m_Promise = Done.m_Promise;

		pRun->m_Config.m_BundleDirectory = _Params["BundleDirectory"].f_String();
		pRun->m_Config.m_CPUCount = (uint32)_Params["CPUCount"].f_AsInteger();
		pRun->m_Config.m_MemoryMB = (uint64)_Params["MemoryMB"].f_AsInteger();
		if (auto *pSharedFolders = _Params.f_GetMember("SharedFolders"))
		{
			for (auto &SharedFolder : pSharedFolders->f_Object())
				pRun->m_Config.m_SharedFolders[SharedFolder.f_Name()] = SharedFolder.f_Value().f_String();
		}

		if (_Params["Backend"].f_AsString() == "MacOSVirtualization")
			pRun->m_Backend = EVirtualizationBackend_MacOSVirtualization;

		pRun->m_Title = _Params["Title"].f_String();

		// The window must run on the process main thread, which waits on the
		// run loop while the command runs in the AppManager
		_pRunLoop->f_Dispatcher()
			(
				[pRun](NConcurrency::CConcurrencyThreadLocal &_ThreadLocal)
				{
					NStr::CStr Error = fg_RunVirtualMachineWindow(pRun->m_Backend, fg_Move(pRun->m_Config), pRun->m_Title);
					if (Error)
						pRun->m_Promise.f_SetException(DMibErrorInstance(Error).f_ExceptionPointer());
					else
						pRun->m_Promise.f_SetResult();
				}
			)
		;

		co_await fg_Move(Done.m_Future);

		co_return {};
	}

	void fg_RegisterVirtualMachineWindowAction()
	{
		// The environment window command runs the virtual machine in the command line
		// client process, where the graphical session of the invoking user is available
		NConcurrency::fg_RegisterCommandLineClientAction("VirtualMachineWindow", &fg_VirtualMachineWindowAction);
	}
}
#endif

struct CAppManager : public CApplication
{
	aint f_Main()
	{
#ifdef DPlatformFamily_macOS
		fg_RegisterVirtualMachineWindowAction();
#endif

		CStr ProgramDirectory = NFile::CFile::fs_GetProgramDirectory();
#ifdef DPlatformFamily_Windows
		CStr DefaultProgramDirectory = "c:/M";
#else
		CStr DefaultProgramDirectory = "/M";
#endif

		CStr DefaultDaemonName = "MalterlibCloudAppManager";
		CStr Description = "Malterlib Cloud App Manager";

		if (ProgramDirectory != DefaultProgramDirectory)
		{
			NCryptography::CHash_SHA256 Hash;

			CStr Salt = "MalterlibAppManagerDaemoName";

			Hash.f_AddData(Salt.f_GetStr(), Salt.f_GetLen());
			Hash.f_AddData(ProgramDirectory.f_GetStr(), ProgramDirectory.f_GetLen());
			DefaultDaemonName = "MalterlibCloudAppManager_{}"_f << Hash.f_GetDigest().f_GetString().f_Left(16);
			Description = "Malterlib Cloud App Manager [{}]"_f << ProgramDirectory;
		}

		NConcurrency::CDistributedDaemon Daemon
			{
				DefaultDaemonName
				, Description
				, "Manages distributed cloud apps running on one host"
				, []
				{
					return fg_ConstructActor<CAppManagerActor>();
				}
			}
		;

		return Daemon.f_Run();
	}
};

DAppImplement(CAppManager);
