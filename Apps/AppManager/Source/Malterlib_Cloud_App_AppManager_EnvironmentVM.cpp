// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Atomic/Atomic>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/RandomID>
#include <Mib/Cryptography/RandomData>
#include <Mib/Daemon/Daemon>

#ifdef DPlatformFamily_macOS
#	include <signal.h>
#endif

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud
{
	// The account name reaches ssh on the host and the account creation in the guest, so only portable account names, which can't be taken
	// for an option, are accepted
	bool fg_AppManager_IsValidVMProvisionUsername(NStr::CStr const &_Username)
	{
		constexpr umint c_MaxLength = 32;

		umint Length = _Username.f_GetLen();
		if (Length == 0 || Length > c_MaxLength)
			return false;

		ch8 const *pUsername = _Username.f_GetStr();
		for (umint iCharacter = 0; iCharacter < Length; ++iCharacter)
		{
			ch8 Character = pUsername[iCharacter];
			if ((Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') || Character == '_')
				continue;

			if (iCharacter > 0 && ((Character >= '0' && Character <= '9') || Character == '-' || Character == '.'))
				continue;

			return false;
		}

		return true;
	}
}

namespace NMib::NCloud::NAppManager
{
	namespace
	{
		CStr fg_GenerateVMMACAddress()
		{
			uint8 Bytes[6];
			NCryptography::fg_GenerateRandomData(Bytes, sizeof(Bytes));

			// Locally administered unicast address
			Bytes[0] = uint8((Bytes[0] | 0x02) & 0xFE);

			ch8 const *pHexDigits = "0123456789abcdef";

			CStr Address;
			for (umint iByte = 0; iByte < 6; ++iByte)
			{
				if (iByte)
					Address += ":";

				ch8 Hex[3] = {pHexDigits[Bytes[iByte] >> 4], pHexDigits[Bytes[iByte] & 0xF], 0};
				Address += Hex;
			}

			return Address;
		}
	}

	CStr CAppManagerActor::fp_GetEnvironmentVMBundleDirectory(CEnvironment const &_Environment)
	{
		CStr VMImagesBaseDirectory = mp_State.m_RootDirectory;
		if (auto pParentApplication = fp_GetEnvironmentParentApplication(_Environment))
			VMImagesBaseDirectory = pParentApplication->f_GetDirectory();

		return fg_Format("{}/{}/{}", VMImagesBaseDirectory, mcp_pVMImagesDirectory, _Environment.m_Settings.m_VMImage);
	}

	NVirtualization::CVirtualMachineConfig CAppManagerActor::fp_BuildEnvironmentVMConfig(CEnvironment &_Environment)
	{
		NVirtualization::CVirtualMachineConfig Config;
		Config.m_BundleDirectory = fp_GetEnvironmentVMBundleDirectory(_Environment);
		Config.m_CPUCount = _Environment.m_Settings.m_VMCPUCount;
		Config.m_MemoryMB = _Environment.m_Settings.m_VMMemoryMB;

		// The guest needs a graphics device to finish the first boot and log the
		// provisioned user in; without one SSH never starts
		Config.m_bGraphics = true;

		// The application storage lives on a persistent data disk managed by the
		// agent inside the guest: virtiofs shares are network volumes gated
		// behind Full Disk Access there, and a local volume also survives when
		// the guest image bundle is recreated
		{
			auto &DataDisk = Config.m_DataDisks.f_Insert();
			DataDisk.m_ImagePath = fp_GetEnvironmentStorageDirectory(_Environment) / "DataDisk.img";
			DataDisk.m_SizeGB = _Environment.m_Settings.m_VMDataDiskGB ? _Environment.m_Settings.m_VMDataDiskGB : 64;
			DataDisk.m_DeviceIdentifier = mcp_pEnvironmentDataVolumeName;
		}

		// The guest reaches the internet but not the host, so its agent and applications reach the environment listen through a virtio socket
		Config.m_Network = NVirtualization::EVirtualMachineNetwork_Isolated;
		Config.m_HostSocketForwards.f_Insert({.m_Port = mcp_VMAgentHostPort, .m_SocketPath = fp_GetEnvironmentAgentSocketPath(_Environment)});
		Config.m_MACAddress = _Environment.m_VMMACAddress;

		return Config;
	}

	TCFuture<NVirtualization::CMacOSGuestProvisioning> CAppManagerActor::fp_LoadVMImageProvisioning(CStr _BundleDirectory)
	{
		CStr Contents;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			CStr ProvisioningPath = fg_Format("{}/Provisioning.json", _BundleDirectory);
			Contents = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [ProvisioningPath]() -> CStr
					{
						if (!CFile::fs_FileExists(ProvisioningPath))
							return {};

						return CFile::fs_ReadStringFromFile(ProvisioningPath);
					}
				)
			;
		}

		NVirtualization::CMacOSGuestProvisioning Provisioning;
		if (Contents.f_IsEmpty())
			co_return Provisioning;

		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Failed to parse image provisioning");

			CEJsonSorted Json = CEJsonSorted::fs_FromString(Contents);

			Provisioning.m_Username = Json["Username"].f_AsString();
			Provisioning.m_Password = Json["Password"].f_AsString();
			Provisioning.m_FullName = Json["FullName"].f_AsString();
			Provisioning.m_bAutoLogin = Json["AutoLogin"].f_AsBoolean(true);
			Provisioning.m_bEnableRemoteLogin = Json["EnableRemoteLogin"].f_AsBoolean(true);
		}

		if (!Provisioning.m_Username.f_IsEmpty() && !fg_AppManager_IsValidVMProvisionUsername(Provisioning.m_Username))
			co_return DMibErrorInstance("The image provisioning names an invalid account '{}'"_f << Provisioning.m_Username);

		co_return Provisioning;
	}

#ifdef DPlatformFamily_macOS
	CEJsonSorted fg_AppManager_BuildVMWindowConfig(NVirtualization::CVirtualMachineConfig const &_Config, CStr const &_Backend, CStr const &_Title)
	{
		CEJsonSorted Json;
		Json["BundleDirectory"] = _Config.m_BundleDirectory;
		Json["Backend"] = _Backend;
		Json["CPUCount"] = (int64)_Config.m_CPUCount;
		Json["MemoryMB"] = (int64)_Config.m_MemoryMB;
		Json["MACAddress"] = _Config.m_MACAddress;
		Json["Isolated"] = _Config.m_Network == NVirtualization::EVirtualMachineNetwork_Isolated;
		Json["Title"] = _Title;

		auto &HostSocketForwards = Json["HostSocketForwards"];
		HostSocketForwards.f_Array();
		for (auto &Forward : _Config.m_HostSocketForwards)
		{
			CEJsonSorted ForwardJson(EJsonType_Object);
			ForwardJson["Port"] = (int64)Forward.m_Port;
			ForwardJson["SocketPath"] = Forward.m_SocketPath;
			HostSocketForwards.f_Insert(fg_Move(ForwardJson));
		}

		auto &SharedFolders = Json["SharedFolders"];
		SharedFolders.f_Object();
		for (auto &SharedFolder : _Config.m_SharedFolders)
			SharedFolders[_Config.m_SharedFolders.fs_GetKey(SharedFolder)] = SharedFolder;

		auto &DataDisks = Json["DataDisks"];
		DataDisks.f_Array();
		for (auto &DataDisk : _Config.m_DataDisks)
		{
			CEJsonSorted DataDiskJson(EJsonType_Object);
			DataDiskJson["ImagePath"] = DataDisk.m_ImagePath;
			DataDiskJson["SizeGB"] = (int64)DataDisk.m_SizeGB;
			DataDiskJson["DeviceIdentifier"] = DataDisk.m_DeviceIdentifier;
			DataDisks.f_Insert(fg_Move(DataDiskJson));
		}

		if (_Config.m_Provisioning)
		{
			auto &Provisioning = Json["Provisioning"];
			Provisioning["Username"] = _Config.m_Provisioning.m_Username;
			Provisioning["Password"] = _Config.m_Provisioning.m_Password;
			Provisioning["FullName"] = _Config.m_Provisioning.m_FullName;
			Provisioning["AutoLogin"] = _Config.m_Provisioning.m_bAutoLogin;
			Provisioning["EnableRemoteLogin"] = _Config.m_Provisioning.m_bEnableRemoteLogin;
		}

		return Json;
	}

	NVirtualization::CVirtualMachineConfig fg_AppManager_ParseVMWindowConfig(CEJsonSorted const &_Params, NVirtualization::EVirtualizationBackend &o_Backend, CStr &o_Title)
	{
		NVirtualization::CVirtualMachineConfig Config;
		Config.m_BundleDirectory = _Params["BundleDirectory"].f_String();
		Config.m_CPUCount = (uint32)_Params["CPUCount"].f_AsInteger();
		Config.m_MemoryMB = (uint64)_Params["MemoryMB"].f_AsInteger();
		Config.m_MACAddress = _Params["MACAddress"].f_AsString();
		if (_Params["Isolated"].f_AsBoolean(false))
			Config.m_Network = NVirtualization::EVirtualMachineNetwork_Isolated;

		if (auto *pHostSocketForwards = _Params.f_GetMember("HostSocketForwards"))
		{
			for (auto &ForwardJson : pHostSocketForwards->f_Array())
			{
				auto &Forward = Config.m_HostSocketForwards.f_Insert();
				Forward.m_Port = (uint32)ForwardJson["Port"].f_AsInteger();
				Forward.m_SocketPath = ForwardJson["SocketPath"].f_AsString();
			}
		}

		if (auto *pSharedFolders = _Params.f_GetMember("SharedFolders"))
		{
			for (auto &SharedFolder : pSharedFolders->f_Object())
				Config.m_SharedFolders[SharedFolder.f_Name()] = SharedFolder.f_Value().f_String();
		}

		if (auto *pDataDisks = _Params.f_GetMember("DataDisks"))
		{
			for (auto &DataDiskJson : pDataDisks->f_Array())
			{
				auto &DataDisk = Config.m_DataDisks.f_Insert();
				DataDisk.m_ImagePath = DataDiskJson["ImagePath"].f_AsString();
				DataDisk.m_SizeGB = (uint64)DataDiskJson["SizeGB"].f_AsInteger();
				DataDisk.m_DeviceIdentifier = DataDiskJson["DeviceIdentifier"].f_AsString();
			}
		}

		if (auto *pProvisioning = _Params.f_GetMember("Provisioning"))
		{
			Config.m_Provisioning.m_Username = (*pProvisioning)["Username"].f_AsString();
			Config.m_Provisioning.m_Password = (*pProvisioning)["Password"].f_AsString();
			Config.m_Provisioning.m_FullName = (*pProvisioning)["FullName"].f_AsString();
			Config.m_Provisioning.m_bAutoLogin = (*pProvisioning)["AutoLogin"].f_AsBoolean(true);
			Config.m_Provisioning.m_bEnableRemoteLogin = (*pProvisioning)["EnableRemoteLogin"].f_AsBoolean(true);
		}

		o_Backend = NVirtualization::EVirtualizationBackend_Default;
		if (_Params["Backend"].f_AsString() == "MacOSVirtualization")
			o_Backend = NVirtualization::EVirtualizationBackend_MacOSVirtualization;

		o_Title = _Params["Title"].f_AsString();

		return Config;
	}

	// Runs the window of --environment-window in the command line process, where the graphical session of the invoking user is
	// available. _Request is the configuration fg_AppManager_BuildVMWindowConfig built, and the request is answered when the window closes
	TCFuture<CEJsonSorted> fg_AppManager_HandleEnvironmentWindowRequest(CEJsonSorted _Params, CEJsonSorted _Request, NStorage::TCSharedPointer<CRunLoop> _pRunLoop)
	{
		using namespace NVirtualization;

		struct CWindowRun
		{
			CVirtualMachineConfig m_Config;
			EVirtualizationBackend m_Backend = EVirtualizationBackend_Default;
			CStr m_Title;
			TCPromise<void> m_Promise;
		};

		TCPromiseFuturePair<void> Done;

		NStorage::TCSharedPointer<CWindowRun> pRun = fg_Construct();
		pRun->m_Promise = Done.m_Promise;
		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Invalid VM window configuration");
			pRun->m_Config = fg_AppManager_ParseVMWindowConfig(_Request, pRun->m_Backend, pRun->m_Title);
		}

		// The window must run on the main thread of the command line process, which waits on the run loop while the command runs in the AppManager
		_pRunLoop->f_Dispatcher()
			(
				[pRun](CConcurrencyThreadLocal &_ThreadLocal)
				{
					CStr Error = fg_RunVirtualMachineWindow(pRun->m_Backend, fg_Move(pRun->m_Config), pRun->m_Title);
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

	namespace
	{
		constexpr ch8 const gc_VMWindowRunningMarker[] = "MalterlibVMWindowRunning";
	}

	// Runs in the separate --vm-window-run process that owns the guest display
	uint32 fg_AppManager_RunVMWindowHost(CStr const &_ConfigPath)
	{
		using namespace NVirtualization;

		CStr Error;
		try
		{
			CEJsonSorted Json = CEJsonSorted::fs_FromString(CFile::fs_ReadStringFromFile(_ConfigPath, true), _ConfigPath);

			EVirtualizationBackend Backend;
			CStr Title;
			CVirtualMachineConfig Config = fg_AppManager_ParseVMWindowConfig(Json, Backend, Title);

			Error = fg_RunVirtualMachineWindow
				(
					Backend
					, fg_Move(Config)
					, Title
					, EVirtualMachineWindowFlag_Host
					, [](CStr const &_GuestAddress)
					{
						// The hosting AppManager waits for this marker before treating
						// the machine as running, and takes the guest address from it
						DMibConOut("{} {}\n", gc_VMWindowRunningMarker, _GuestAddress);
					}
				)
			;
		}
		catch (CException const &_Exception)
		{
			Error = "Failed to run the virtual machine window host: {}"_f << _Exception;
		}

		if (!Error.f_IsEmpty())
		{
			DMibConErrOut("{}\n", Error);
			return 1;
		}

		return 0;
	}

	namespace
	{
		struct CVMWindowHostRunningMarkerActor : public NProcess::CProcessLaunchActor
		{
			CVMWindowHostRunningMarkerActor(TCPromise<CStr> const &_RunningPromise);

		protected:
			bool fp_WillFilterOutput() override;
			void fp_FilterOutput(NProcess::EProcessLaunchOutputType _OutputType, CStr &o_Output) override;

		private:
			TCPromise<CStr> mp_RunningPromise; /// Gets the guest address
			bool mp_bRunning = false;
		};

		CVMWindowHostRunningMarkerActor::CVMWindowHostRunningMarkerActor(TCPromise<CStr> const &_RunningPromise)
			: mp_RunningPromise(_RunningPromise)
		{
		}

		bool CVMWindowHostRunningMarkerActor::fp_WillFilterOutput()
		{
			return true;
		}

		void CVMWindowHostRunningMarkerActor::fp_FilterOutput(NProcess::EProcessLaunchOutputType _OutputType, CStr &o_Output)
		{
			if (_OutputType != NProcess::EProcessLaunchOutputType_StdOut)
				return;

			aint iMarker = o_Output.f_Find(gc_VMWindowRunningMarker);
			if (mp_bRunning || iMarker < 0)
				return;

			mp_bRunning = true;

			CStr GuestAddress = o_Output.f_Extract(iMarker + aint(sizeof(gc_VMWindowRunningMarker) - 1));
			if (aint iLineEnd = GuestAddress.f_FindChar('\n'); iLineEnd >= 0)
				GuestAddress = GuestAddress.f_Left(iLineEnd);

			mp_RunningPromise.f_SetResult(CStr(GuestAddress.f_Trim()));
		}

		// Proxies the virtual machine to the window host process, which runs it and shows its display
		struct CVMWindowHostActor : public NVirtualization::CVirtualMachineActor
		{
			CVMWindowHostActor(CStr &&_ConfigPath, CStr &&_WorkingDirectory, CStr &&_LogName);

			TCFuture<void> f_Start() override;
			TCFuture<void> f_Stop() override;
			TCFuture<bool> f_WaitForStop(fp64 _TimeoutSeconds) override;
			TCFuture<void> f_ForceStop() override;
			TCFuture<NVirtualization::EVirtualMachineState> f_GetState() override;
			TCFuture<NVirtualization::EVirtualMachineCapability> f_GetCapabilities() override;
			TCFuture<CStr> f_GetGuestAddress() override;

		protected:
			TCFuture<void> fp_Destroy() override;

		private:
			CStr mp_ConfigPath;
			CStr mp_GuestAddress;
			CStr mp_WorkingDirectory;
			CStr mp_LogName;
			TCActor<NProcess::CProcessLaunchActor> mp_Launch;
			CActorSubscription mp_LaunchSubscription;
			TCFuture<void> mp_ExitFuture;        /// Ready once the host exits with its machine or fails to launch
			NVirtualization::EVirtualMachineState mp_State = NVirtualization::EVirtualMachineState_Stopped;
			bool mp_bHostLaunchFinished = false; /// There is no host process to signal at stop
		};

		CVMWindowHostActor::CVMWindowHostActor(CStr &&_ConfigPath, CStr &&_WorkingDirectory, CStr &&_LogName)
			: mp_ConfigPath(fg_Move(_ConfigPath))
			, mp_WorkingDirectory(fg_Move(_WorkingDirectory))
			, mp_LogName(fg_Move(_LogName))
		{
		}

		TCFuture<void> CVMWindowHostActor::f_Start()
		{
			using namespace NVirtualization;

			if (mp_State == EVirtualMachineState_Running || mp_State == EVirtualMachineState_Starting)
				co_return {};

			mp_State = EVirtualMachineState_Starting;
			mp_bHostLaunchFinished = false;

			// A window host that survived an earlier AppManager instance keeps the
			// machine's auxiliary storage locked, so stale hosts for this
			// configuration are stopped, best effort, before a new one launches
			{
				auto BlockingActorCheckout = fg_BlockingActor();

				co_await
					(
						g_Dispatch(BlockingActorCheckout) / [ConfigPath = mp_ConfigPath]()
						{
							CStr ProgramPath = CFile::fs_GetProgramPath();

							CProcessLaunch::fs_KillProcesses
								(
									[&](NProcess::CProcessInfo const &_ProcessInfo) -> bool
									{
										if (_ProcessInfo.m_FullPath != ProgramPath)
											return false;

										for (auto const &Arg : _ProcessInfo.m_Args)
										{
											if (Arg == ConfigPath)
												return true;
										}

										return false;
									}
									, NProcess::EProcessInfoFlag_FullPath | NProcess::EProcessInfoFlag_Args
								)
							;
						}
					)
					.f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop a stale virtual machine window host")
				;
			}

			TCPromiseFuturePair<CStr> RunningPromise;
			TCPromiseFuturePair<void> ExitPromise;

			mp_Launch = fg_ConstructActor<CVMWindowHostRunningMarkerActor>(RunningPromise.m_Promise);
			mp_ExitFuture = fg_Move(ExitPromise.m_Future);

			auto Promise = RunningPromise.m_Promise;
			auto HostExitPromise = ExitPromise.m_Promise;
			CProcessLaunchActor::CLaunch Launch = CProcessLaunchParams::fs_LaunchExecutable
				(
					CFile::fs_GetProgramPath()
					, fg_CreateVector<CStr>("--vm-window-run", "--config", mp_ConfigPath)
					, mp_WorkingDirectory
					, [this, Promise, HostExitPromise](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
					{
						switch (_State.f_GetTypeID())
						{
						case NProcess::EProcessLaunchState_Launched: break;
						case NProcess::EProcessLaunchState_LaunchFailed:
							{
								auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
								mp_bHostLaunchFinished = true;
								if (!Promise.f_IsSet())
									Promise.f_SetException(DMibErrorInstance("Failed to launch the window host: {}"_f << LaunchError).f_ExceptionPointer());
								if (!HostExitPromise.f_IsSet())
									HostExitPromise.f_SetResult();
							}
							break;
						case NProcess::EProcessLaunchState_Exited:
							{
								auto ExitStatus = _State.f_Get<NProcess::EProcessLaunchState_Exited>();
								mp_bHostLaunchFinished = true;
								if (!Promise.f_IsSet())
									Promise.f_SetException(DMibErrorInstance("The window host exited with '{}' before the machine started"_f << ExitStatus).f_ExceptionPointer());
								if (!HostExitPromise.f_IsSet())
									HostExitPromise.f_SetResult();
							}
							break;
						}
					}
				)
			;
			Launch.m_ToLog = CProcessLaunchActor::ELogFlag_All;
			Launch.m_LogName = mp_LogName;

			auto LaunchSubscription = co_await fg_TempCopy(mp_Launch)(&CProcessLaunchActor::f_Launch, Launch, fg_ThisActor(this)).f_Wrap();
			if (!LaunchSubscription)
			{
				mp_State = EVirtualMachineState_Failed;
				co_return LaunchSubscription.f_GetException();
			}

			mp_LaunchSubscription = fg_Move(*LaunchSubscription);

			auto Result = co_await fg_Move(RunningPromise.m_Future).f_Timeout(120.0, "Timed out waiting for the virtual machine window host to start the machine").f_Wrap();

			if (!Result)
			{
				mp_State = EVirtualMachineState_Failed;
				co_return Result.f_GetException();
			}

			mp_GuestAddress = fg_Move(*Result);
			mp_State = EVirtualMachineState_Running;

			co_return {};
		}

		TCFuture<void> CVMWindowHostActor::f_Stop()
		{
			using namespace NVirtualization;

			if (!mp_Launch)
			{
				mp_State = EVirtualMachineState_Stopped;
				co_return {};
			}

			mp_State = EVirtualMachineState_Stopping;

			if (!mp_bHostLaunchFinished)
			{
				// The termination signal asks the guest to shut down, and the host exits once the guest has stopped
				co_await fg_TempCopy(mp_Launch)(&CProcessLaunchActor::f_StopProcess).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop the virtual machine window host");
			}

			co_await fg_Move(mp_Launch).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the virtual machine window host launch");

			mp_LaunchSubscription.f_Clear();
			mp_State = EVirtualMachineState_Stopped;

			co_return {};
		}

		TCFuture<bool> CVMWindowHostActor::f_WaitForStop(fp64 _TimeoutSeconds)
		{
			using namespace NVirtualization;

			if (!mp_Launch || !mp_ExitFuture.f_IsValid())
				co_return true;

			// The window host exits by itself once its machine stops
			auto Result = co_await fg_Move(mp_ExitFuture).f_Timeout(_TimeoutSeconds, "Timed out waiting for the virtual machine to stop").f_Wrap();

			co_return (bool)Result;
		}

		TCFuture<void> CVMWindowHostActor::f_ForceStop()
		{
			using namespace NVirtualization;

			if (!mp_Launch)
			{
				mp_State = EVirtualMachineState_Stopped;
				co_return {};
			}

			mp_State = EVirtualMachineState_Stopping;

			if (!mp_bHostLaunchFinished)
			{
				// Killing the host kills the machine with it
				co_await fg_TempCopy(mp_Launch)(&CProcessLaunchActor::f_Signal, int32(SIGKILL)).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to kill the virtual machine window host")
				;

				co_await fg_TempCopy(mp_Launch)(&CProcessLaunchActor::f_StopProcess).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop the virtual machine window host");
			}

			co_await fg_Move(mp_Launch).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the virtual machine window host launch");

			mp_LaunchSubscription.f_Clear();
			mp_State = EVirtualMachineState_Stopped;

			co_return {};
		}

		TCFuture<NVirtualization::EVirtualMachineState> CVMWindowHostActor::f_GetState()
		{
			co_return mp_State;
		}

		TCFuture<NVirtualization::EVirtualMachineCapability> CVMWindowHostActor::f_GetCapabilities()
		{
			using namespace NVirtualization;

			co_return EVirtualMachineCapability_SharedFolders | EVirtualMachineCapability_MacOSGuests;
		}

		TCFuture<CStr> CVMWindowHostActor::f_GetGuestAddress()
		{
			co_return mp_GuestAddress;
		}

		TCFuture<void> CVMWindowHostActor::fp_Destroy()
		{
			if (mp_Launch)
				co_await f_Stop().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop the virtual machine window host at destroy");

			co_return co_await CActor::fp_Destroy();
		}
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_VMSettings(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto Auditor = f_Auditor({}, CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/VMSettings"}};

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Change VM settings in AppManager", Permissions, CallingHostInfo) % "Permission denied changing VM settings" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(VM settings, command)", Permissions["Command"]);

		bool bWindow = mp_bVMWindow;
		if (auto *pValue = _Params.f_GetMember("Window"))
			bWindow = pValue->f_AsBoolean(false);

		if (bWindow == mp_bVMWindow)
		{
			co_await _pCommandLine->f_StdOut("Window: {}\n"_f << (mp_bVMWindow ? "true" : "false"));
			co_return 0;
		}

		// The window setting selects the host process at launch, and a running VM cannot move to another one. A VM that a window client hosts
		// keeps its window until the client closes it
		TCVector<TCSharedPointer<CEnvironment>> RestartEnvironments;
		TCVector<CStr> RestartNames;
		for (auto &pEnvironment : mp_Environments)
		{
			if (pEnvironment->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_VM || pEnvironment->m_bWindowOpen)
				continue;

			if (!pEnvironment->f_IsStarted() && !pEnvironment->m_bStarting)
				continue;

			RestartEnvironments.f_Insert(fg_TempCopy(pEnvironment));
			RestartNames.f_Insert(pEnvironment->m_Name);
		}

		co_await fp_CheckEnvironmentRestartAccess(fg_Move(RestartNames), "Restart environments to apply VM settings in AppManager", CallingHostInfo);

		mp_bVMWindow = bWindow;
		mp_State.m_StateDatabase.m_Data["VMSettings"]["Window"] = mp_bVMWindow;

		co_await mp_State.m_StateDatabase.f_Save();

		// The changed setting is saved and the environments restart to apply it also when the client is gone
		bool bConnected = co_await fp_CommandLineProgress(_pCommandLine, true, "Window: {}\n"_f << (mp_bVMWindow ? "true" : "false"), false);

		co_return co_await fp_RestartEnvironmentsForSettings(fg_Move(RestartEnvironments), _pCommandLine, bConnected);
	}

#endif

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

		CStr BundleDirectory = fp_GetEnvironmentVMBundleDirectory(*_pEnvironment);

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

		if (mp_CreatingVMImageBundles.f_FindEqual(BundleDirectory.f_LowerCase()))
		{
			CStr Error = "Cannot start environment '{}': VM image bundle '{}' is being created"_f << _pEnvironment->m_Name << BundleDirectory;
			_pEnvironment->f_SetStatus(Error, CAppManagerInterface::EStatusSeverity_Error);
			co_return DMibErrorInstance(Error);
		}

		// A stop, a removal or the shutdown asks the start to give up, which it checks after each step that suspends until the machine starts
		auto fCheckAbort = [&]() -> CExceptionPointer
			{
				if (_pEnvironment->m_bDeleted)
					return DMibErrorInstance("Environment has been deleted").f_ExceptionPointer();

				if (_pEnvironment->m_bRemoving || _pEnvironment->m_bStartAbortRequested || mp_bStoppingApp)
					return DMibErrorInstance("The start of environment '{}' was aborted"_f << _pEnvironment->m_Name).f_ExceptionPointer();

				return nullptr;
			}
		;

		_pEnvironment->f_SetStatus("Starting VM", CAppManagerInterface::EStatusSeverity_Warning);

		NWeb::NHTTP::CURL AgentAddress = co_await (fp_EnsureEnvironmentListen(_pEnvironment) % "Failed to add environment listen");
		if (auto pException = fCheckAbort())
			co_return pException;

		// Without agent files there is nothing to install in the guest, so the start fails before the VM boots
		TCSharedPointer<CStr> pError = fg_Construct();
		CStr AgentExecutable = co_await fp_GetEnvironmentAgentExecutable(_pEnvironment, pError);
		if (auto pException = fCheckAbort())
			co_return pException;

		if (AgentExecutable.f_IsEmpty())
		{
			_pEnvironment->f_SetStatus(*pError, CAppManagerInterface::EStatusSeverity_Error);
			co_return DMibErrorInstance(*pError);
		}

		{
			auto BlockingActorCheckout = fg_BlockingActor();

			_pEnvironment->m_VMAgentDesiredFingerprint = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [AgentExecutable]() -> CStr
					{
						// An update can change these files without changing the executable
						CStr AgentDirectory = CFile::fs_GetPath(AgentExecutable);
						CStr ExecutableName = CFile::fs_GetFile(AgentExecutable);

						CStr Fingerprint;
						for (auto &File : {ExecutableName, CStr("MalterlibHelper"), CFile::fs_GetFileNoExt(ExecutableName) + "VersionInfo.json", CStr("AppManagerSettings.json")})
						{
							if (!CFile::fs_FileExists(AgentDirectory / File))
								continue;

							Fingerprint += "{}:{}:{}\n"_f << File << CFile::fs_GetFileSize(AgentDirectory / File) << CFile::fs_GetWriteTime(AgentDirectory / File).f_GetSeconds();
						}

						return Fingerprint;
					}
				)
			;
		}

		if (auto pException = fCheckAbort())
			co_return pException;

		// Guest provisioning stored with the image is applied by the guest on its first boot after restore and ignored afterwards
		CMacOSGuestProvisioning Provisioning = co_await fp_LoadVMImageProvisioning(BundleDirectory);
		if (auto pException = fCheckAbort())
			co_return pException;

		bool bCanBootstrap = (bool)Provisioning && Provisioning.m_bEnableRemoteLogin;

		// Reuse the installed launch ID only when the sizes and second-resolution write times of the agent files match; a new ID keeps the installed agent from
		// registering by it. Without provisioning the host can't install the agent, so the agent already installed in the guest keeps its ID. The
		// host name isn't part of the match: an installed agent keeps the name it was installed with until the host installs the agent again
		if (!_pEnvironment->m_VMAgentLaunchID.f_IsEmpty() && (!bCanBootstrap || _pEnvironment->m_VMAgentFingerprint == _pEnvironment->m_VMAgentDesiredFingerprint))
			_pEnvironment->m_LaunchID = _pEnvironment->m_VMAgentLaunchID;
		else
			_pEnvironment->m_LaunchID = fg_RandomID();

		// The guest agent starts as a daemon without the standard stream handshake container agents use, so it connects with a ticket
		CStr ConnectSettings;
		_pEnvironment->m_AgentTicketSubscription.f_Clear();
		{
			auto Ticket = co_await mp_State.m_TrustManager
				(
					&CDistributedActorTrustManager::f_GenerateConnectionTicket
					, AgentAddress
					, g_ActorFunctor / [_pEnvironment](CStr _HostID, CCallingHostInfo _HostInfo, CByteVector _CertificateRequest) -> TCFuture<void>
					{
						if (_pEnvironment->m_bDeleted)
							co_return DMibErrorInstance("Environment deleted");

						_pEnvironment->m_AgentHostID = _HostID;

						co_return {};
					}
					, nullptr
				)
			;

			_pEnvironment->m_AgentTicketSubscription = fg_Move(Ticket.m_NotificationsSubscription);

			if (auto pException = fCheckAbort())
				co_return pException;

			// The agent forwards the socket of the address in the guest to the virtio socket port that the host forwards to the listen
			CEJsonSorted ConnectJson;
			ConnectJson["Address"] = AgentAddress.f_Encode();
			ConnectJson["LaunchID"] = _pEnvironment->m_LaunchID;
			ConnectJson["Ticket"] = CStr(Ticket.m_Ticket.f_ToStringTicket());
			ConnectJson["HostPort"] = int64(mcp_VMAgentHostPort);
			ConnectJson["SocketPath"] = fp_GetEnvironmentAgentSocketPath(*_pEnvironment);

			// The agent applies it when it starts, before its distributed identity reads the name; see fp_GetEnvironmentHostName
			{
				CStr HostNameError;
				ConnectJson["EnvironmentHostName"] = fp_GetEnvironmentHostName(*_pEnvironment, HostNameError);
				if (!HostNameError.f_IsEmpty())
				{
					_pEnvironment->f_SetStatus(HostNameError, CAppManagerInterface::EStatusSeverity_Error);
					co_return DMibErrorInstance(HostNameError);
				}
			}

			ConnectSettings = ConnectJson.f_ToString();
		}

		// A stable MAC address keeps the network identity of the guest across restarts
		if (_pEnvironment->m_VMMACAddress.f_IsEmpty())
		{
			_pEnvironment->m_VMMACAddress = fg_GenerateVMMACAddress();

			co_await fp_UpdateEnvironmentJson(_pEnvironment).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to save the environment MAC address");
		}

		CVirtualMachineConfig Config = fp_BuildEnvironmentVMConfig(*_pEnvironment);
		Config.m_Provisioning = Provisioning;

		// An agent that disconnected leaves its virtual machine running on the same guest disk, so it is stopped, best effort,
		// before the new machine starts
		if (_pEnvironment->m_VMActor)
		{
			co_await fg_TempCopy(_pEnvironment->m_VMActor)(&NVirtualization::CVirtualMachineActor::f_Stop).f_Wrap()
				> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop the previous environment virtual machine")
			;

			co_await fg_Move(_pEnvironment->m_VMActor).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the previous environment virtual machine");

			if (auto pException = fCheckAbort())
				co_return pException;
		}

#ifdef DPlatformFamily_macOS
		bool bWindow = mp_bVMWindow;
		if (bWindow && !fg_HasGraphicalSession())
		{
			bWindow = false;

			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Warning
					, "The VM window setting is on but no graphical session is available; starting environment '{}' without a window"
					, _pEnvironment->m_Name
				)
			;
		}

		if (bWindow)
		{
			// The window host runs with the privileges of the AppManager, so applications must not be able to write this configuration
			CStr ConfigPath = mp_State.m_RootDirectory / "VMWindow" / ("{}.json"_f << _pEnvironment->m_Name);
			CStr ConfigContents = fg_AppManager_BuildVMWindowConfig(Config, Settings.m_VMBackend, fg_Format("AppManager Environment '{}'", _pEnvironment->m_Name)).f_ToString();

			{
				auto BlockingActorCheckout = fg_BlockingActor();

				co_await
					(
						g_Dispatch(BlockingActorCheckout)
						/ [ConfigPath, ConfigContents]()
						{
							// Holds the guest administrator password
							CFile::fs_CreateDirectory(CFile::fs_GetPath(ConfigPath));
							CFile::fs_WriteStringToFile(ConfigPath, ConfigContents, true, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UnixAttributesValid);
						}
						% "Failed to write the window host configuration"
					)
				;
			}

			if (auto pException = fCheckAbort())
				co_return pException;

			_pEnvironment->m_VMActor = fg_ConstructActor<CVMWindowHostActor>(fg_Move(ConfigPath), CStr(mp_State.m_RootDirectory), "Environment/{}/VMWindow"_f << _pEnvironment->m_Name);
		}
		else
#endif
		{
			_pEnvironment->m_VMActor = fg_CreateVirtualMachine(Backend, fg_Move(Config));
		}

		auto StartResult = co_await _pEnvironment->m_VMActor(&CVirtualMachineActor::f_Start).f_Wrap();

		if (!StartResult)
		{
			_pEnvironment->f_SetStatus(fg_Format("Failed to start VM: {}", StartResult.f_GetExceptionStr()), CAppManagerInterface::EStatusSeverity_Error);
			fp_StopEnvironmentInternal(_pEnvironment) > fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop environment '{}' after its start failed"_f << _pEnvironment->m_Name);
			co_return StartResult.f_GetException();
		}

		// The stop, removal or shutdown that asked the start to give up stops the machine
		if (auto pException = fCheckAbort())
			co_return pException;

		_pEnvironment->f_SetStatus("VM running, setting up the agent", CAppManagerInterface::EStatusSeverity_Warning);

		if (!_pEnvironment->f_IsStarted())
		{
			// Without provisioning the host can't install the agent, so the start waits for an agent that is already installed in the guest, like
			// one that an earlier start with provisioning set up, to boot with the guest and connect
			if (!bCanBootstrap)
			{
				_pEnvironment->f_SetStatus("VM running, waiting for the agent installed in the guest", CAppManagerInterface::EStatusSeverity_Warning);

				auto ConnectedResult = co_await _pEnvironment->m_OnAgentConnected.f_Insert()
					.f_Future()
					.f_Timeout
					(
						600.0
						, "Timed out waiting for the agent installed in the VM guest to connect. The VM image carries no provisioning credentials with "
						"remote login to install one"
					)
					.f_Wrap()
				;

				if (!ConnectedResult && !_pEnvironment->f_IsStarted())
				{
					if (_pEnvironment->m_bStartAbortRequested)
						co_return ConnectedResult.f_GetException();

					CStr Error = "Cannot set up the agent for environment '{}': {}"_f << _pEnvironment->m_Name << ConnectedResult.f_GetExceptionStr();
					_pEnvironment->f_SetStatus(Error, CAppManagerInterface::EStatusSeverity_Error);
					fp_StopEnvironmentInternal(_pEnvironment)
						> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop environment '{}' after its start failed"_f << _pEnvironment->m_Name)
					;
					co_return DMibErrorInstance(Error);
				}

				co_return {};
			}

			auto BootstrapResult = co_await fp_BootstrapVMAgentOverSSH(_pEnvironment, Provisioning, AgentExecutable, ConnectSettings).f_Wrap();

			if (!BootstrapResult && !_pEnvironment->f_IsStarted())
			{
				// The stop, removal or shutdown that asked the start to give up sets the status and stops the machine
				if (_pEnvironment->m_bStartAbortRequested)
					co_return BootstrapResult.f_GetException();

				CStr Error = fg_Format("Failed to set up the environment agent through SSH: {}", BootstrapResult.f_GetExceptionStr());
				_pEnvironment->f_SetStatus(Error, CAppManagerInterface::EStatusSeverity_Error);
				fp_StopEnvironmentInternal(_pEnvironment) > fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop environment '{}' after its start failed"_f << _pEnvironment->m_Name);
				co_return BootstrapResult.f_GetException();
			}

			if (!_pEnvironment->f_IsStarted())
			{
				if (_pEnvironment->m_bStartAbortRequested)
					co_return DMibErrorInstance("Environment '{}' is stopping"_f << _pEnvironment->m_Name);

				_pEnvironment->f_SetStatus("Agent installed, waiting for agent", CAppManagerInterface::EStatusSeverity_Warning);

				auto ConnectedResult = co_await _pEnvironment->m_OnAgentConnected.f_Insert()
					.f_Future()
					.f_Timeout(120.0, "Timed out waiting for the environment agent in the VM to connect")
					.f_Wrap()
				;

				if (!ConnectedResult && !_pEnvironment->f_IsStarted())
				{
					if (_pEnvironment->m_bStartAbortRequested)
						co_return ConnectedResult.f_GetException();

					CStr GuestLog = co_await fp_FetchVMAgentGuestLog(_pEnvironment, Provisioning);

					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "The agent for environment '{}' did not connect; guest agent log:\n{}", _pEnvironment->m_Name, GuestLog);

					_pEnvironment->f_SetStatus(fg_Format("Agent failed to connect: {}", ConnectedResult.f_GetExceptionStr()), CAppManagerInterface::EStatusSeverity_Error);
					fp_StopEnvironmentInternal(_pEnvironment)
						> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop environment '{}' after its start failed"_f << _pEnvironment->m_Name)
					;
					co_return ConnectedResult.f_GetException();
				}
			}
		}

		co_return {};
	}

	namespace
	{
		struct CVMBootstrapOutputCaptureActor : public NProcess::CProcessLaunchActor
		{
			struct CState
			{
				NThread::CMutual m_Lock;
				CStr m_Output;
			};

			CVMBootstrapOutputCaptureActor(NStorage::TCSharedPointer<CState> const &_pState);

		protected:
			bool fp_WillFilterOutput() override;
			void fp_FilterOutput(NProcess::EProcessLaunchOutputType _OutputType, CStr &o_Output) override;

		private:
			NStorage::TCSharedPointer<CState> mp_pState;
		};

		CVMBootstrapOutputCaptureActor::CVMBootstrapOutputCaptureActor(NStorage::TCSharedPointer<CState> const &_pState)
			: mp_pState(_pState)
		{
		}

		bool CVMBootstrapOutputCaptureActor::fp_WillFilterOutput()
		{
			return true;
		}

		void CVMBootstrapOutputCaptureActor::fp_FilterOutput(NProcess::EProcessLaunchOutputType _OutputType, CStr &o_Output)
		{
			DMibLock(mp_pState->m_Lock);
			mp_pState->m_Output += o_Output;
		}

		struct CVMBootstrapRunResult
		{
			uint32 m_ExitCode = 0;
			CStr m_Output;
		};

		// Keep the daemon on the guest boot disk so it starts without the application storage being mounted
		constexpr ch8 const gc_VMAgentGuestDirectory[] = "/opt/Malterlib/Agent";
		constexpr ch8 const gc_VMAgentDaemonName[] = "MalterlibAppManagerAgent";

		// The files that run as root are staged in a directory only the SSH user can access: other guest users could replace
		// files in a shared location such as /tmp before sudo runs them. Expanded by the guest shell, so it stays quoted
		constexpr ch8 const gc_VMBootstrapStageDirectory[] = "\"$HOME/.MalterlibAgentBootstrap\"";

		// The bootstrap sends the guest administrator password and an agent ticket, so the host key the guest of the image
		// presents first is pinned in the image bundle and required afterwards; the alias keeps it across guest addresses
		// The guest only takes the provisioned password, so the configuration, keys and agent of the account the AppManager runs as are left
		// out: offered keys count against the authentication attempts of the guest before the password is tried
		TCVector<CStr> fg_CreateVMBootstrapSSHOptions(CStr const &_BundleDirectory)
		{
			return fg_CreateVector<CStr>
				(
					"-F"
					, "/dev/null"
					, "-o"
					, "PubkeyAuthentication=no"
					, "-o"
					, "IdentityAgent=none"
					, "-o"
					, "PreferredAuthentications=keyboard-interactive,password"
					, "-o"
					, "StrictHostKeyChecking=accept-new"
					// ssh splits the option value at whitespace unless it is quoted
					, "-o"
					, "UserKnownHostsFile=\"{}\""_f << (_BundleDirectory / ".BootstrapKnownHosts")
					, "-o"
					, "HostKeyAlias=MalterlibEnvironmentGuest"
					, "-o"
					, "GlobalKnownHostsFile=/dev/null"
					, "-o"
					, "ConnectTimeout=10"
					, "-o"
					, "NumberOfPasswordPrompts=1"
					, "-o"
					, "ServerAliveInterval=15"
					, "-o"
					, "ServerAliveCountMax=4"
					, "-o"
					, "LogLevel=ERROR"
				)
			;
		}

		// Runs one command against the guest, feeding the input once the process has launched: input sent before is silently dropped
		auto fg_RunVMBootstrapSSH
			(
				TCActor<CActor> _CallbackActor
				, CStr _LogName
				, CStr _WorkingDirectory
				, bool _bLogToStdErr
				, CStr _AskPassPath
				, CStr _Password
				, CStr _Executable
				, TCVector<CStr> _Arguments
				, CStrIO _StdInData
				, fp64 _Timeout
				, TCFunction<bool ()> _fAborted
			)
			-> TCFuture<CVMBootstrapRunResult>
		{
			NStorage::TCSharedPointer<CVMBootstrapOutputCaptureActor::CState> pOutputState = fg_Construct();

			TCPromiseFuturePair<void> LaunchedPromise;
			TCPromiseFuturePair<uint32> ExitPromise;

			CProcessLaunchActor::CLaunch Launch = CProcessLaunchParams::fs_LaunchExecutable
				(
					fg_Move(_Executable)
					, _Arguments
					, _WorkingDirectory
					, [Launched = LaunchedPromise.m_Promise, Exited = ExitPromise.m_Promise](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
					{
						switch (_State.f_GetTypeID())
						{
						case NProcess::EProcessLaunchState_Launched:
							if (!Launched.f_IsSet())
								Launched.f_SetResult();
							break;
						case NProcess::EProcessLaunchState_LaunchFailed:
							{
								auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();

								auto pException = DMibErrorInstance("Failed to launch SSH: {}"_f << LaunchError).f_ExceptionPointer();
								if (!Launched.f_IsSet())
									Launched.f_SetException(pException);
								if (!Exited.f_IsSet())
									Exited.f_SetException(pException);
							}
							break;
						case NProcess::EProcessLaunchState_Exited:
							{
								uint32 ExitCode = _State.f_Get<NProcess::EProcessLaunchState_Exited>();

								if (!Launched.f_IsSet())
									Launched.f_SetResult();
								if (!Exited.f_IsSet())
									Exited.f_SetResult(ExitCode);
							}
							break;
						}
					}
				)
			;

			auto &LaunchParams = Launch.m_Params;
			LaunchParams.m_Environment["SSH_ASKPASS"] = _AskPassPath;
			LaunchParams.m_Environment["SSH_ASKPASS_REQUIRE"] = "force";
			LaunchParams.m_Environment["DISPLAY"] = ":0";
			LaunchParams.m_Environment["MALTERLIB_VM_SSH_PASSWORD"] = _Password;
			LaunchParams.m_bMergeEnvironment = true;

			Launch.m_LogName = fg_Move(_LogName);
			Launch.m_ToLog = CProcessLaunchActor::ELogFlag_All;
			if (_bLogToStdErr)
				Launch.m_ToLog |= CProcessLaunchActor::ELogFlag_AdditionallyOutputToStdErr;

			TCActor<NProcess::CProcessLaunchActor> LaunchActor = fg_ConstructActor<CVMBootstrapOutputCaptureActor>(pOutputState);

			auto LaunchSubscription = co_await fg_TempCopy(LaunchActor)(&CProcessLaunchActor::f_Launch, Launch, _CallbackActor).f_Wrap();
			if (!LaunchSubscription)
			{
				co_await fg_Move(LaunchActor).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the agent setup launch");

				co_return LaunchSubscription.f_GetException();
			}

			auto LaunchedResult = co_await fg_Move(LaunchedPromise.m_Future).f_Timeout(30.0, "Timed out waiting for SSH to launch").f_Wrap();

			if (LaunchedResult)
			{
				co_await fg_TempCopy(LaunchActor)(&CProcessLaunchActor::f_SendStdIn, fg_Move(_StdInData)).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to send the agent setup input")
				;
				co_await fg_TempCopy(LaunchActor)(&CProcessLaunchActor::f_CloseStdIn).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to close the agent setup input");
			}

			// A stop of the environment doesn't wait for the attempt to time out. The exit is reported on _CallbackActor, which runs this
			bool bAborted = false;
			bool bTimedOut = false;
			if (LaunchedResult)
			{
				auto ExitedCheck = ExitPromise.m_Promise;
				NTime::CStopwatch Stopwatch{true};
				while (!ExitedCheck.f_IsSet())
				{
					if (_fAborted())
					{
						bAborted = true;
						break;
					}

					if (Stopwatch.f_GetTime() >= _Timeout)
					{
						bTimedOut = true;
						break;
					}

					co_await fg_Timeout(0.5);
				}
			}

			TCAsyncResult<uint32> ExitResult;
			if (bAborted)
				ExitResult.f_SetException(DMibErrorInstance("Environment is stopping"));
			else if (bTimedOut)
				ExitResult.f_SetException(DMibErrorInstance("The SSH agent setup attempt timed out"));
			else if (LaunchedResult)
				ExitResult = co_await fg_Move(ExitPromise.m_Future).f_Wrap();

			co_await fg_Move(LaunchActor).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the agent setup launch");

			if (!LaunchedResult)
				co_return LaunchedResult.f_GetException();

			if (!ExitResult)
				co_return ExitResult.f_GetException();

			CVMBootstrapRunResult Result;
			Result.m_ExitCode = *ExitResult;

			{
				DMibLock(pOutputState->m_Lock);
				Result.m_Output = fg_Move(pOutputState->m_Output);
			}

			co_return Result;
		}
	}

	// The helper runs with the privileges of the AppManager, so it must not live in storage an application can write,
	// like the directory of a parent application
	CStr CAppManagerActor::fp_GetVMBootstrapAskPassPath(CEnvironment const &_Environment)
	{
		return mp_State.m_RootDirectory / "VMBootstrap" / ("{}.AskPass.sh"_f << _Environment.m_Name);
	}

	auto CAppManagerActor::fp_BootstrapVMAgentOverSSH
		(
			TCSharedPointer<CEnvironment> _pEnvironment
			, NVirtualization::CMacOSGuestProvisioning _Provisioning
			, CStr _AgentExecutable
			, CStr _ConnectSettings
		)
		-> TCFuture<void>
	{
		// The isolated network reserves the guest address, so it is known once the machine runs
		CStr GuestIP = co_await _pEnvironment->m_VMActor(&NVirtualization::CVirtualMachineActor::f_GetGuestAddress);
		if (GuestIP.f_IsEmpty())
			co_return DMibErrorInstance("The virtual machine has no guest address");

		// The ask pass helper lets ssh take the password from the process
		// environment instead of a terminal prompt
		CStr AskPassPath = fp_GetVMBootstrapAskPassPath(*_pEnvironment);
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					g_Dispatch(BlockingActorCheckout)
					/ [AskPassPath]()
					{
						CFile::fs_CreateDirectory(CFile::fs_GetPath(AskPassPath));
						CFile::fs_WriteStringToFile(AskPassPath, "#!/bin/sh\nprintf '%s\\n' \"$MALTERLIB_VM_SSH_PASSWORD\"\n", false, EFileAttrib_Executable);
					}
					% "Failed to write the SSH ask pass helper"
				)
			;
		}

		CStr AgentDirectory = CFile::fs_GetPath(_AgentExecutable);
		CStr ExecutableName = CFile::fs_GetFile(_AgentExecutable);

		// The bootstrap compares these files between the staged and the installed agent
		TCVector<CStr> AgentFiles;
		{
			TCVector<CStr> Candidates = fg_CreateVector<CStr>(ExecutableName, "MalterlibHelper", CFile::fs_GetFileNoExt(ExecutableName) + "VersionInfo.json", "AppManagerSettings.json");

			auto BlockingActorCheckout = fg_BlockingActor();

			AgentFiles = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [AgentDirectory, Candidates = fg_Move(Candidates)]()
					{
						TCVector<CStr> Files;
						for (auto &Candidate : Candidates)
						{
							if (CFile::fs_FileExists(AgentDirectory / Candidate))
								Files.f_Insert(fg_TempCopy(Candidate));
						}

						return Files;
					}
				)
			;
		}

		CStr QuotedExecutableName = NStr::fg_StrEscapeBashSingleQuotes(ExecutableName);

		CStr AgentFileList;
		for (auto &File : AgentFiles)
		{
			if (!AgentFileList.f_IsEmpty())
				AgentFileList += " ";
			AgentFileList += NStr::fg_StrEscapeBashSingleQuotes(File);
		}

		// The agent reads its connect settings when it starts, so a kept agent restarts when the address, the launch ID or the host name changes.
		// The ticket changes with every start and is not part of the identity
		CStr ConnectIdentity;
		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Failed to parse the agent connect settings");

			CEJsonSorted ConnectJson = CEJsonSorted::fs_FromString(_ConnectSettings);
			CStr IdentityData = ConnectJson["Address"].f_String() + "\n" + ConnectJson["LaunchID"].f_String() + "\n" + ConnectJson["EnvironmentHostName"].f_AsString();
			ConnectIdentity = NCryptography::CHash_SHA256::fs_DigestFromData(IdentityData.f_GetStr(), IdentityData.f_GetLen()).f_GetString();
		}

		// A matching agent whose daemon is loaded is kept, and only its connect settings are refreshed
		CStr BootstrapScript = fg_Format
			(
				"set -e\n"
				"StageDir=\"$1\"\n"
				"(umask 077; cat > \"$StageDir/AgentConnect.json\") <<'MalterlibConnectEnd'\n"
				"{}\n"
				"MalterlibConnectEnd\n"
				"AgentDir='{}'\n"
				"NewDir=\"$AgentDir.bootstrap\"\n"
				"rm -rf \"$NewDir\"\n"
				"mkdir -p \"$NewDir\"\n"
				"tar -xf \"$StageDir/AgentFiles.tar\" -C \"$NewDir\"\n"
				"rm -f \"$StageDir/AgentFiles.tar\"\n"
				"MalterlibSame=1\n"
				"for MalterlibFile in {}; do\n"
				"\tif ! cmp -s \"$NewDir/$MalterlibFile\" \"$AgentDir/$MalterlibFile\"; then\n"
				"\t\tMalterlibSame=0\n"
				"\tfi\n"
				"done\n"
				"if [ \"$(cat \"$AgentDir/AppManagerAgentConnectIdentity\" 2>/dev/null || true)\" != '{}' ]; then\n"
				"\tMalterlibSame=0\n"
				"fi\n"
				"if [ \"$MalterlibSame\" = \"1\" ] && launchctl print 'system/{}' >/dev/null 2>&1; then\n"
				"\trm -rf \"$NewDir\"\n"
				"\tumask 077\n"
				"\tcp \"$StageDir/AgentConnect.json\" \"$AgentDir/AppManagerAgentConnect.json\"\n"
				"\techo MalterlibAgentKept\n"
				"\texit 0\n"
				"fi\n"
				"if [ -x \"$AgentDir/\"{} ]; then\n"
				"\t\"$AgentDir/\"{} --daemon-stop --mode global '{}' >/dev/null 2>&1 || true\n"
				"fi\n"
				"mkdir -p \"$AgentDir\"\n"
				"for MalterlibFile in {}; do\n"
				"\tmv -f \"$NewDir/$MalterlibFile\" \"$AgentDir/$MalterlibFile\"\n"
				"done\n"
				"rm -rf \"$NewDir\"\n"
				"umask 077\n"
				"cp \"$StageDir/AgentConnect.json\" \"$AgentDir/AppManagerAgentConnect.json\"\n"
				"printf '%s' '{}' > \"$AgentDir/AppManagerAgentConnectIdentity\"\n"
				"cd \"$AgentDir\"\n"
				"./{} --daemon-add --mode global --run-as-user root --run-as-group wheel --no-fail-if-added '{}'\n"
				, _ConnectSettings
				, gc_VMAgentGuestDirectory
				, AgentFileList
				, ConnectIdentity
				, "{}.{}"_f << NDaemon::CDaemon::fs_GetUniquePrefix() << gc_VMAgentDaemonName
				, QuotedExecutableName
				, QuotedExecutableName
				, gc_VMAgentDaemonName
				, AgentFileList
				, ConnectIdentity
				, QuotedExecutableName
				, gc_VMAgentDaemonName
			)
		;

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "Setting up the agent for environment '{}' through SSH as user '{}' at {}"
				, _pEnvironment->m_Name
				, _Provisioning.m_Username
				, GuestIP
			)
		;

		auto fAborted = [this, _pEnvironment]
			{
				return _pEnvironment->m_bDeleted || _pEnvironment->m_bRemoving || _pEnvironment->m_bStopping || _pEnvironment->m_bStartAbortRequested || mp_bStoppingApp;
			}
		;

		// SSH may not accept connections until the guest finishes its provisioning first boot
		CStr LastError = "The guest did not accept SSH connections";
		for (umint Attempt = 0; Attempt < 90; ++Attempt)
		{
			if (Attempt)
				co_await fg_Timeout(5.0);

			if (_pEnvironment->f_IsStarted())
				co_return {};

			if (fAborted())
				co_return DMibErrorInstance("Environment is stopping");

			CStr LogName = "Environment/{}/AgentBootstrap"_f << _pEnvironment->m_Name;

			// Follows a "--" in every ssh invocation, so that ssh never takes it for an option
			CStr Target = fg_Format("{}@{}", _Provisioning.m_Username, GuestIP);

			TCVector<CStr> CommonOptions = fg_CreateVMBootstrapSSHOptions(fp_GetEnvironmentVMBundleDirectory(*_pEnvironment));

			// The bootstrap script contains the connection ticket; stage it only in the SSH user's private directory
			CStr Error;
			{
				CStr Pipeline = "tar -cf - -C {}"_f << NStr::fg_StrEscapeBashSingleQuotes(AgentDirectory);
				for (auto &File : AgentFiles)
					Pipeline += " " + NStr::fg_StrEscapeBashSingleQuotes(File);

				Pipeline += " | exec /usr/bin/ssh";
				for (auto &Option : CommonOptions)
					Pipeline += " " + NStr::fg_StrEscapeBashSingleQuotes(Option);
				Pipeline += " -- " + NStr::fg_StrEscapeBashSingleQuotes(Target);
				Pipeline += " " + NStr::fg_StrEscapeBashSingleQuotes(CStr("umask 077 && rm -rf {0} && mkdir {0} && cat > {0}/AgentFiles.tar"_f << gc_VMBootstrapStageDirectory));

				auto TransferResult = co_await fg_RunVMBootstrapSSH
					(
						fg_ThisActor(this)
						, LogName
						, mp_State.m_RootDirectory
						, mp_bLogLaunchesToStdErr
						, AskPassPath
						, _Provisioning.m_Password
						, "/bin/sh"
						, fg_CreateVector<CStr>("-c", fg_Move(Pipeline))
						, CStrIO()
						, 300.0
						, fAborted
					)
					.f_Wrap()
				;

				if (!TransferResult)
					Error = TransferResult.f_GetExceptionStr();
				else if (TransferResult->m_ExitCode != 0)
					Error = fg_Format("Transferring the agent files failed with status {}: {}", TransferResult->m_ExitCode, CStr(TransferResult->m_Output.f_Trim()));
			}

			if (Error.f_IsEmpty())
			{
				TCVector<CStr> Arguments = CommonOptions;
				Arguments.f_Insert("--");
				Arguments.f_Insert(Target);
				Arguments.f_Insert(fg_Format("cat > {}/Bootstrap.sh", gc_VMBootstrapStageDirectory));

				CStrIO ScriptInput;
				ScriptInput += BootstrapScript;

				auto StageResult = co_await fg_RunVMBootstrapSSH
					(
						fg_ThisActor(this)
						, LogName
						, mp_State.m_RootDirectory
						, mp_bLogLaunchesToStdErr
						, AskPassPath
						, _Provisioning.m_Password
						, "/usr/bin/ssh"
						, fg_Move(Arguments)
						, fg_Move(ScriptInput)
						, 60.0
						, fAborted
					)
					.f_Wrap()
				;

				if (!StageResult)
					Error = StageResult.f_GetExceptionStr();
				else if (StageResult->m_ExitCode != 0)
					Error = fg_Format("Staging the setup script failed with status {}: {}", StageResult->m_ExitCode, CStr(StageResult->m_Output.f_Trim()));
			}

			if (Error.f_IsEmpty())
			{
				TCVector<CStr> Arguments = CommonOptions;
				Arguments.f_Insert("--");
				Arguments.f_Insert(Target);
				Arguments.f_Insert(fg_Format("sudo -S -p '' /bin/sh {0}/Bootstrap.sh {0}; MalterlibStatus=$?; rm -rf {0}; exit $MalterlibStatus", gc_VMBootstrapStageDirectory));

				// Only sudo reads the input, and it does not when it needs no password, so the connect settings travel in the
				// staged script
				CStrIO PasswordInput;
				PasswordInput += _Provisioning.m_Password;
				PasswordInput += "\n";

				auto RunResult = co_await fg_RunVMBootstrapSSH
					(
						fg_ThisActor(this)
						, LogName
						, mp_State.m_RootDirectory
						, mp_bLogLaunchesToStdErr
						, AskPassPath
						, _Provisioning.m_Password
						, "/usr/bin/ssh"
						, fg_Move(Arguments)
						, fg_Move(PasswordInput)
						, 180.0
						, fAborted
					)
					.f_Wrap()
				;

				if (!RunResult)
					Error = RunResult.f_GetExceptionStr();
				else if (RunResult->m_ExitCode != 0)
					Error = fg_Format("SSH exited with status {}: {}", RunResult->m_ExitCode, CStr(RunResult->m_Output.f_Trim()));
				else
				{
					if (_pEnvironment->m_VMAgentLaunchID != _pEnvironment->m_LaunchID || _pEnvironment->m_VMAgentFingerprint != _pEnvironment->m_VMAgentDesiredFingerprint)
					{
						_pEnvironment->m_VMAgentLaunchID = _pEnvironment->m_LaunchID;
						_pEnvironment->m_VMAgentFingerprint = _pEnvironment->m_VMAgentDesiredFingerprint;

						co_await fp_UpdateEnvironmentJson(_pEnvironment).f_Wrap() > fg_LogWarning("Malterlib/Cloud/AppManager", "Failed to persist the environment agent launch id");
					}

					bool bKept = RunResult->m_Output.f_Find("MalterlibAgentKept") >= 0;

					DMibLogWithCategory
						(
							Malterlib/Cloud/AppManager
							, Info
							, "{} the agent for environment '{}' in the guest at {}"
							, bKept ? "Kept" : "Installed"
							, _pEnvironment->m_Name
							, GuestIP
						)
					;

					co_return {};
				}
			}

			if (Error != LastError)
			{
				LastError = Error;

				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Agent setup attempt for environment '{}' failed, retrying: {}", _pEnvironment->m_Name, Error);

				CStr StatusError = Error;
				if (auto iNewLine = StatusError.f_FindChar('\n'); iNewLine >= 0)
					StatusError = StatusError.f_Left(iNewLine);
				StatusError = StatusError.f_Left(160);

				_pEnvironment->f_SetStatus("Setting up the environment agent through SSH: {}"_f << StatusError, CAppManagerInterface::EStatusSeverity_Warning);
			}
		}

		co_return DMibErrorInstance("Failed to set up the agent in the guest: {}"_f << LastError);
	}

	TCFuture<CStr> CAppManagerActor::fp_FetchVMAgentGuestLog(TCSharedPointer<CEnvironment> _pEnvironment, NVirtualization::CMacOSGuestProvisioning _Provisioning)
	{
		if (!_pEnvironment->m_VMActor)
			co_return CStr("The virtual machine is not running");

		auto GuestIPResult = co_await _pEnvironment->m_VMActor(&NVirtualization::CVirtualMachineActor::f_GetGuestAddress).f_Wrap();
		if (!GuestIPResult)
			co_return CStr(fg_Format("Failed to get the guest address: {}", GuestIPResult.f_GetExceptionStr()));

		CStr GuestIP = fg_Move(*GuestIPResult);
		if (GuestIP.f_IsEmpty())
			co_return CStr("The virtual machine has no guest address");

		TCVector<CStr> Arguments = fg_CreateVMBootstrapSSHOptions(fp_GetEnvironmentVMBundleDirectory(*_pEnvironment));
		Arguments.f_Insert("--");
		Arguments.f_Insert(fg_Format("{}@{}", _Provisioning.m_Username, GuestIP));
		// The guest agent log is readable only by root
		for (auto pWord : {"sudo", "-S", "-p", "''", "tail", "-n", "60"})
			Arguments.f_Insert(pWord);
		Arguments.f_Insert(fg_Format("{}/Log/AppManager.log", gc_VMAgentGuestDirectory));
		Arguments.f_Insert("2>&1");

		CStrIO PasswordInput;
		PasswordInput += _Provisioning.m_Password;
		PasswordInput += "\n";

		auto Result = co_await fg_RunVMBootstrapSSH
			(
				fg_ThisActor(this)
				, "Environment/{}/AgentGuestLog"_f << _pEnvironment->m_Name
				, mp_State.m_RootDirectory
				, false
				, fp_GetVMBootstrapAskPassPath(*_pEnvironment)
				, _Provisioning.m_Password
				, "/usr/bin/ssh"
				, fg_Move(Arguments)
				, fg_Move(PasswordInput)
				, 30.0
				, []
				{
					return false;
				}
			)
			.f_Wrap()
		;

		if (!Result)
			co_return CStr(fg_Format("Failed to fetch the guest agent log: {}", Result.f_GetExceptionStr()));

		co_return fg_Move(Result->m_Output);
	}

#ifdef DPlatformFamily_macOS
	TCFuture<void> CAppManagerActor::fp_EndEnvironmentWindowSession(TCSharedPointer<CEnvironment> _pEnvironment, bool _bWasStarted)
	{
		_pEnvironment->m_bWindowOpen = false;

		if (!_bWasStarted || _pEnvironment->m_bStoppedManually)
		{
			if (!_pEnvironment->f_IsStarted() && !_pEnvironment->m_bStarting)
				_pEnvironment->f_SetStatus("Not started", CAppManagerInterface::EStatusSeverity_Warning);

			co_return {};
		}

		co_await fp_EnsureEnvironmentStarted(_pEnvironment);
		fp_UpdateApplicationDependencies();

		co_return {};
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_EnvironmentWindow(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		using namespace NVirtualization;

		CStr Name = _Params["Name"].f_String();

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto Auditor = f_Auditor({}, CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/EnvironmentWindow"}};
		Permissions["Environment"] =
			{
				CPermissionQuery{"AppManager/EnvironmentAll", fg_Format("AppManager/Environment/{}", Name)}.f_Description("Access environment {} in AppManager"_f << Name)
			}
		;

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Open environment window in AppManager", Permissions, CallingHostInfo) % "Permission denied opening environment window" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Environment window, command)", Permissions["Command"]);

		if (!HasPermissions["Environment"])
			co_return Auditor.f_AccessDenied("(Environment window, environment name)", Permissions["Environment"]);

		// The window runs in the command line client, which takes over hosting the machine, so an older client is refused before the environment stops
		if (!_pCommandLine->f_SupportsRequest())
		{
			co_await _pCommandLine->f_StdErr
				(
					"The VM window needs a command line client with protocol version 0x{nh} or later; update the client\n"_f << uint32(ICCommandLineControl::EProtocolVersion_SupportRequest)
				)
			;
			co_return 1;
		}

		auto *pFindEnvironment = mp_Environments.f_FindEqual(Name);
		if (!pFindEnvironment)
		{
			co_await _pCommandLine->f_StdErr("No such environment '{}'\n"_f << Name);
			co_return 1;
		}

		auto pEnvironment = *pFindEnvironment;

		if (pEnvironment->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_VM)
		{
			co_await _pCommandLine->f_StdErr("Environment '{}' is not a VM environment\n"_f << Name);
			co_return 1;
		}

		CStr BundleDirectory = fp_BuildEnvironmentVMConfig(*pEnvironment).m_BundleDirectory;

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
			co_await _pCommandLine->f_StdErr("VM image bundle '{}' does not exist\n"_f << BundleDirectory);
			co_return 1;
		}

		if (mp_CreatingVMImageBundles.f_FindEqual(BundleDirectory.f_LowerCase()))
		{
			co_await _pCommandLine->f_StdErr("VM image bundle '{}' is being created\n"_f << BundleDirectory);
			co_return 1;
		}

		// The environment can be removed, or change its image, while the bundle is checked; the reservation below prevents both
		if (pEnvironment->m_bDeleted || pEnvironment->m_bRemoving || fp_BuildEnvironmentVMConfig(*pEnvironment).m_BundleDirectory != BundleDirectory)
		{
			co_await _pCommandLine->f_StdErr("Environment '{}' was removed or changed its VM image while the window opened\n"_f << Name);
			co_return 1;
		}

		if (pEnvironment->m_bWindowOpen)
		{
			co_await _pCommandLine->f_StdErr("Environment '{}' is already open in a window\n"_f << Name);
			co_return 1;
		}

		// A stop in progress, like one of a restart or of a failed start, runs the machine until it finishes, which the stop below waits for
		bool bWasStarted = pEnvironment->f_IsStarted() || pEnvironment->m_bStarting || pEnvironment->m_bStopping || pEnvironment->m_VMActor;

		// Reserve the image before the stop suspends, so that another start cannot boot it in a second VM
		pEnvironment->m_bWindowOpen = true;

		// Every exit, even after a failed window session, releases the image and tries to start an environment that was running or starting and not stopped manually
		auto EndWindowSession = co_await fg_AsyncDestroy
			(
				[this, pEnvironment, bWasStarted]() -> TCFuture<void>
				{
					auto pLocalEnvironment = pEnvironment;

					auto Result = co_await fp_EndEnvironmentWindowSession(pLocalEnvironment, bWasStarted).f_Wrap();
					if (!Result)
					{
						DMibLogWithCategory
							(
								Malterlib/Cloud/AppManager
								, Error
								, "Failed to start environment '{}' after the window session: {}"
								, pLocalEnvironment->m_Name
								, Result.f_GetExceptionStr()
							)
						;
					}

					co_return {};
				}
			)
		;

		// The window session hosts the virtual machine in the command line client
		// process instead of the AppManager, so the environment is stopped first to
		// release the guest image bundle
		if (bWasStarted)
		{
			co_await _pCommandLine->f_StdOut("Stopping environment '{}'\n"_f << Name);

			co_await fp_WaitForEnvironmentIdle(pEnvironment);
			auto StopResult = co_await fp_StopEnvironmentInternal(pEnvironment).f_Wrap();
			if (!StopResult)
				co_return _pCommandLine->f_AddAsyncResult(StopResult);
		}

		// Lets an agent already installed in the guest reconnect during the window session
		co_await (fp_EnsureEnvironmentListen(pEnvironment) % "Failed to add environment listen");

		pEnvironment->f_SetStatus("Environment window open", CAppManagerInterface::EStatusSeverity_Warning);

		// Settings other than the image, like the memory, can change while the environment stops
		CVirtualMachineConfig Config = fp_BuildEnvironmentVMConfig(*pEnvironment);
		Config.m_Provisioning = co_await fp_LoadVMImageProvisioning(Config.m_BundleDirectory);

		CEJsonSorted WindowConfig = fg_AppManager_BuildVMWindowConfig(Config, pEnvironment->m_Settings.m_VMBackend, fg_Format("AppManager Environment '{}'", Name));

		co_await _pCommandLine->f_StdOut
			(
				"Opening a window for environment '{}'\n"
				"Closing the window asks the guest to shut down; closing it again forces the stop\n"_f
				<< Name
			)
		;

		auto WindowResult = co_await _pCommandLine->f_Request(fg_Move(WindowConfig)).f_Wrap();

		EndWindowSession.f_Clear();

		auto EndResult = co_await fp_EndEnvironmentWindowSession(pEnvironment, bWasStarted).f_Wrap();
		if (!EndResult)
		{
			if (WindowResult)
				co_return _pCommandLine->f_AddAsyncResult(EndResult);

			co_await _pCommandLine->f_StdErr("Failed to start environment '{}' after the window session: {}\n"_f << Name << EndResult.f_GetExceptionStr());
		}

		co_return _pCommandLine->f_AddAsyncResult(WindowResult);
	}
#endif

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_CreateVMImage(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		using namespace NVirtualization;

		CStr Name = _Params["Name"].f_String();
		CStr RestoreImage = _Params["RestoreImage"].f_String();

		// The name becomes a directory in the VM image storage
		if (!CVersionManager::fs_IsValidApplicationName(Name))
		{
			co_await _pCommandLine->f_StdErr("'{}' is not a valid VM image name\n"_f << Name);
			co_return 1;
		}

		if (CStr ProvisionUsername = _Params["ProvisionUsername"].f_AsString(); !ProvisionUsername.f_IsEmpty() && !fg_AppManager_IsValidVMProvisionUsername(ProvisionUsername))
		{
			co_await _pCommandLine->f_StdErr("'{}' is not a valid account name for the guest\n"_f << ProvisionUsername);
			co_return 1;
		}

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto Auditor = f_Auditor({}, CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/VMImageCreate"}};

		// The image is created in the directory of the parent application
		CStr ParentApplication = _Params["ParentApplication"].f_String();
		if (!ParentApplication.f_IsEmpty())
		{
			Permissions["ParentApplication"] =
				{
					CPermissionQuery{"AppManager/AppAll", "AppManager/App/{}"_f << ParentApplication}.f_Description("Access application {} in AppManager"_f << ParentApplication)
				}
			;
		}

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Create VM image in AppManager", Permissions, CallingHostInfo) % "Permission denied creating VM image" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(VM image create, command)", Permissions["Command"]);

		if (auto *pHasPermission = HasPermissions.f_FindEqual("ParentApplication"); pHasPermission && !*pHasPermission)
			co_return Auditor.f_AccessDenied("(VM image create, parent application)", Permissions["ParentApplication"]);

		if (!fg_IsVirtualizationBackendAvailable(EVirtualizationBackend_Default))
		{
			co_await _pCommandLine->f_StdErr("No virtualization backend is available on this host\n");
			co_return 1;
		}

		CStr BaseDirectory = mp_State.m_RootDirectory;

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

			// The VM backend opens the image bundle on the host, where such an application has no files
			if (fp_ApplicationRemoteStorageEnvironment(**pFindApplication))
			{
				co_await _pCommandLine->f_StdErr("Parent application '{}' keeps its files in environment-managed storage\n"_f << ParentApplication);
				co_return 1;
			}

			BaseDirectory = (*pFindApplication)->f_GetDirectory();
		}

		CStr BundleDirectory = fg_Format("{}/{}/{}", BaseDirectory, mcp_pVMImagesDirectory, Name);

		// Environments that name the image start only once it is created and its provisioning is stored, since the first boot applies it
		CStr BundleReservation = BundleDirectory.f_LowerCase();
		if (mp_CreatingVMImageBundles.f_FindEqual(BundleReservation))
		{
			co_await _pCommandLine->f_StdErr("The VM image '{}' is being created\n"_f << Name);
			co_return 1;
		}

		mp_CreatingVMImageBundles[BundleReservation];
		auto EndReservation = g_OnScopeExit / [this, BundleReservation]
			{
				mp_CreatingVMImageBundles.f_Remove(BundleReservation);
			}
		;

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
		CreateParams.m_MemoryMB = (uint64)_Params["VMMemoryMB"].f_AsInteger();
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
				> [Promise, pDone](TCAsyncResult<void> &&_Result) mutable
				{
					*pDone = true;
					if (!_Result)
						Promise.f_SetException(_Result.f_GetException());
					else
						Promise.f_SetResult();
				}
			;
		}

		// The install goes on when the client disconnects, so the provisioning below has to be stored also when reporting the progress fails
		fp64 LastReportedProgress = -1.0;
		bool bReportProgress = true;
		while (!*pDone)
		{
			co_await fg_Timeout(2.0);

			fp64 Progress = *pProgress;
			if (bReportProgress && Progress > LastReportedProgress)
			{
				LastReportedProgress = Progress;
				auto OutputResult = co_await _pCommandLine->f_StdOut("Installing: {}%\n"_f << aint((Progress * 100.0).f_Get())).f_Wrap();
				if (!OutputResult)
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Stopped reporting the install progress of VM image '{}': {}", Name, OutputResult.f_GetExceptionStr());
					bReportProgress = false;
				}
			}
		}

		auto Result = co_await fg_Move(DonePromise.m_Future).f_Wrap();

		// Provisioning is stored with the image, so whichever boot comes first applies it
		CStr ProvisionUsername = _Params["ProvisionUsername"].f_AsString();
		if (Result && !ProvisionUsername.f_IsEmpty())
		{
			CStr ProvisionPassword = _Params["ProvisionPassword"].f_AsString();
			if (ProvisionPassword.f_IsEmpty())
				ProvisionPassword = NCryptography::fg_RandomID(20);

			CEJsonSorted Provisioning;
			Provisioning["Username"] = ProvisionUsername;
			Provisioning["Password"] = ProvisionPassword;
			Provisioning["FullName"] = _Params["ProvisionFullName"].f_AsString();
			Provisioning["AutoLogin"] = _Params["ProvisionAutoLogin"].f_AsBoolean(true);
			Provisioning["EnableRemoteLogin"] = _Params["ProvisionEnableSSH"].f_AsBoolean(true);

			auto BlockingActorCheckout = fg_BlockingActor();

			CStr ProvisioningPath = fg_Format("{}/Provisioning.json", BundleDirectory);
			CStr Contents = Provisioning.f_ToString();
			co_await
				(
					g_Dispatch(BlockingActorCheckout)
					/ [ProvisioningPath, Contents]()
					{
						// Holds the guest administrator password
						CFile::fs_WriteStringToFile(ProvisioningPath, Contents, true, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UnixAttributesValid);
					}
					% "Failed to write image provisioning"
				)
			;

			co_await _pCommandLine->f_StdOut
				(
					"Stored guest provisioning for user '{}' with password '{}'\n"
					"It is applied on the first boot of the image, which also sets up the environment agent through SSH\n"_f
					<< ProvisionUsername
					<< ProvisionPassword
				)
			;
		}

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}
}
