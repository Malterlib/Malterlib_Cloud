// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Atomic/Atomic>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/RandomID>
#include <Mib/Cryptography/RandomData>

#ifdef DPlatformFamily_macOS
#include <signal.h>
#endif

#include "Malterlib_Cloud_App_AppManager.h"

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
			for (umint i = 0; i < 6; ++i)
			{
				if (i)
					Address += ":";

				ch8 Hex[3] = {pHexDigits[Bytes[i] >> 4], pHexDigits[Bytes[i] & 0xF], 0};
				Address += Hex;
			}

			return Address;
		}

		bool fg_ParseMACOctets(ch8 const *_pStr, umint _Len, uint8 (&o_Octets)[6])
		{
			umint iOctet = 0;
			umint i = 0;
			while (iOctet < 6)
			{
				uint32 Value = 0;
				umint nDigits = 0;
				for (; i < _Len && nDigits < 2; ++i, ++nDigits)
				{
					ch8 Char = _pStr[i];
					if (Char >= '0' && Char <= '9')
						Value = Value * 16 + umint(Char - '0');
					else if (Char >= 'a' && Char <= 'f')
						Value = Value * 16 + umint(Char - 'a' + 10);
					else if (Char >= 'A' && Char <= 'F')
						Value = Value * 16 + umint(Char - 'A' + 10);
					else
						break;
				}

				if (!nDigits)
					return false;

				o_Octets[iOctet++] = uint8(Value);

				if (iOctet < 6)
				{
					if (i >= _Len || _pStr[i] != ':')
						return false;
					++i;
				}
			}

			return iOctet == 6 && i == _Len;
		}

		// Finds the IPv4 address the shared network DHCP server leased to the MAC
		// address; the leases file stores the octets without leading zeros
		CStr fg_FindVMDHCPLease(CStr const &_MACAddress)
		{
			uint8 TargetOctets[6];
			if (!fg_ParseMACOctets(_MACAddress.f_GetStr(), _MACAddress.f_GetLen(), TargetOctets))
				return {};

			CStr const LeasesPath = "/var/db/dhcpd_leases";
			if (!CFile::fs_FileExists(LeasesPath))
				return {};

			CStr Contents = CFile::fs_ReadStringFromFile(LeasesPath);

			ch8 const *pContents = Contents.f_GetStr();
			umint Len = Contents.f_GetLen();

			CStr EntryIP;
			umint iLineStart = 0;
			for (umint i = 0; i <= Len; ++i)
			{
				if (i < Len && pContents[i] != '\n')
					continue;

				umint iStart = iLineStart;
				umint iEnd = i;
				iLineStart = i + 1;

				while (iStart < iEnd && (pContents[iStart] == ' ' || pContents[iStart] == '\t'))
					++iStart;
				while (iEnd > iStart && (pContents[iEnd - 1] == ' ' || pContents[iEnd - 1] == '\r'))
					--iEnd;

				CStr Line = Contents.f_Extract(iStart, iEnd - iStart);

				if (Line == "{")
				{
					EntryIP = {};
					continue;
				}

				if (Line.f_StartsWith("ip_address="))
				{
					EntryIP = Line.f_Extract(11, Line.f_GetLen() - 11);
					continue;
				}

				if (Line.f_StartsWith("hw_address=1,"))
				{
					CStr EntryMAC = Line.f_Extract(13, Line.f_GetLen() - 13);

					uint8 EntryOctets[6];
					if (!fg_ParseMACOctets(EntryMAC.f_GetStr(), EntryMAC.f_GetLen(), EntryOctets))
						continue;

					bool bMatch = true;
					for (umint iOctet = 0; iOctet < 6; ++iOctet)
					{
						if (EntryOctets[iOctet] != TargetOctets[iOctet])
							bMatch = false;
					}

					if (bMatch && !EntryIP.f_IsEmpty())
						return EntryIP;
				}
			}

			return {};
		}
	}

	CStr CAppManagerActor::fp_GetEnvironmentVMBundleDirectory(CEnvironment const &_Environment)
	{
		CStr VMImagesBaseDirectory = mp_State.m_RootDirectory;
		if (auto pParentApplication = fp_GetEnvironmentParentApplication(_Environment))
			VMImagesBaseDirectory = pParentApplication->f_GetDirectory();

		return fg_Format("{}/VMImages/{}", VMImagesBaseDirectory, _Environment.m_Settings.m_VMImage);
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

		// Each application directory is its own shared folder, mounted inside the
		// guest by the agent as the user the application runs as
		_Environment.m_VMShareTags.f_Clear();
		for (auto &pApplication : mp_Applications)
		{
			if (pApplication->m_bDeleted || pApplication->m_Settings.m_LaunchEnvironment != _Environment.m_Name)
				continue;

			CStr Directory = pApplication->f_GetDirectory();
			CStr Tag = fsp_GetVMShareTag(Directory);
			Config.m_SharedFolders[Tag] = Directory;
			_Environment.m_VMShareTags[fg_Move(Directory)] = fg_Move(Tag);
		}

		return Config;
	}

	CStr CAppManagerActor::fsp_GetVMShareTag(CStr const &_Directory)
	{
		// virtiofs limits tags to 36 bytes, so the tag is a digest of the host path
		return "Mib{}"_f << NCryptography::CHash_SHA256::fs_DigestFromData(_Directory.f_GetStr(), _Directory.f_GetLen()).f_GetString().f_Left(30);
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

		co_return Provisioning;
	}

#ifdef DPlatformFamily_macOS
	CEJsonSorted fg_AppManager_BuildVMWindowConfig
		(
			NVirtualization::CVirtualMachineConfig const &_Config
			, CStr const &_Backend
			, CStr const &_Title
		)
	{
		CEJsonSorted Json;
		Json["BundleDirectory"] = _Config.m_BundleDirectory;
		Json["Backend"] = _Backend;
		Json["CPUCount"] = (int64)_Config.m_CPUCount;
		Json["MemoryMB"] = (int64)_Config.m_MemoryMB;
		Json["MACAddress"] = _Config.m_MACAddress;
		Json["Title"] = _Title;

		auto &SharedFolders = Json["SharedFolders"];
		SharedFolders.f_Object();
		for (auto &SharedFolder : _Config.m_SharedFolders)
			SharedFolders[_Config.m_SharedFolders.fs_GetKey(SharedFolder)] = SharedFolder;

		// Guest provisioning stored with the image is applied by the guest on its
		// first boot after restore and ignored afterwards
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

	NVirtualization::CVirtualMachineConfig fg_AppManager_ParseVMWindowConfig
		(
			CEJsonSorted const &_Params
			, NVirtualization::EVirtualizationBackend &o_Backend
			, CStr &o_Title
		)
	{
		NVirtualization::CVirtualMachineConfig Config;
		Config.m_BundleDirectory = _Params["BundleDirectory"].f_String();
		Config.m_CPUCount = (uint32)_Params["CPUCount"].f_AsInteger();
		Config.m_MemoryMB = (uint64)_Params["MemoryMB"].f_AsInteger();
		Config.m_MACAddress = _Params["MACAddress"].f_AsString();

		if (auto *pSharedFolders = _Params.f_GetMember("SharedFolders"))
		{
			for (auto &SharedFolder : pSharedFolders->f_Object())
				Config.m_SharedFolders[SharedFolder.f_Name()] = SharedFolder.f_Value().f_String();
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

	namespace
	{
		constexpr ch8 const gc_pVMWindowRunningMarker[] = "MalterlibVMWindowRunning";
	}

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
					, []
					{
						// The hosting AppManager waits for this marker before treating
						// the machine as running
						DMibConOut("{}\n", gc_pVMWindowRunningMarker);
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
		// Detects the running marker the window host prints once its machine started
		struct CVMWindowHostLaunchActor : public NProcess::CProcessLaunchActor
		{
			CVMWindowHostLaunchActor(TCPromise<void> const &_RunningPromise)
				: mp_RunningPromise(_RunningPromise)
			{
			}

		protected:
			bool fp_WillFilterOutput() override
			{
				return true;
			}

			void fp_FilterOutput(NProcess::EProcessLaunchOutputType _OutputType, CStr &o_Output) override
			{
				if (_OutputType != NProcess::EProcessLaunchOutputType_StdOut)
					return;

				if (!mp_bRunning && o_Output.f_Find(gc_pVMWindowRunningMarker) >= 0)
				{
					mp_bRunning = true;
					mp_RunningPromise.f_SetResult();
				}
			}

		private:
			TCPromise<void> mp_RunningPromise;
			bool mp_bRunning = false;
		};

		// Hosts the environment virtual machine in a separate window process, keeping
		// the guest display visible in the graphical session while the AppManager
		// controls the machine through the process lifetime
		struct CVMWindowHostActor : public NVirtualization::CVirtualMachineActor
		{
			CVMWindowHostActor(CStr &&_ConfigPath, CStr &&_WorkingDirectory, CStr &&_LogName)
				: mp_ConfigPath(fg_Move(_ConfigPath))
				, mp_WorkingDirectory(fg_Move(_WorkingDirectory))
				, mp_LogName(fg_Move(_LogName))
			{
			}

			TCFuture<void> f_Start() override
			{
				using namespace NVirtualization;

				if (mp_State == EVirtualMachineState_Running || mp_State == EVirtualMachineState_Starting)
					co_return {};

				mp_State = EVirtualMachineState_Starting;

				TCPromiseFuturePair<void> RunningPromise;
				TCPromiseFuturePair<void> ExitPromise;

				mp_Launch = fg_ConstructActor<CVMWindowHostLaunchActor>(RunningPromise.m_Promise);
				mp_ExitFuture = fg_Move(ExitPromise.m_Future);

				auto Promise = RunningPromise.m_Promise;
				auto HostExitPromise = ExitPromise.m_Promise;
				CProcessLaunchActor::CLaunch Launch = CProcessLaunchParams::fs_LaunchExecutable
					(
						CFile::fs_GetProgramPath()
						, fg_CreateVector<CStr>("--vm-window-run", "--config", mp_ConfigPath)
						, mp_WorkingDirectory
						, [Promise, HostExitPromise](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
						{
							switch (_State.f_GetTypeID())
							{
							case NProcess::EProcessLaunchState_Launched:
								break;
							case NProcess::EProcessLaunchState_LaunchFailed:
								{
									auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
									if (!Promise.f_IsSet())
										Promise.f_SetException(DMibErrorInstance("Failed to launch the window host: {}"_f << LaunchError).f_ExceptionPointer());
									if (!HostExitPromise.f_IsSet())
										HostExitPromise.f_SetResult();
								}
								break;
							case NProcess::EProcessLaunchState_Exited:
								{
									auto ExitStatus = _State.f_Get<NProcess::EProcessLaunchState_Exited>();
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

				auto Result = co_await fg_Move(RunningPromise.m_Future)
					.f_Timeout(120.0, "Timed out waiting for the virtual machine window host to start the machine")
					.f_Wrap()
				;

				if (!Result)
				{
					mp_State = EVirtualMachineState_Failed;
					co_return Result.f_GetException();
				}

				mp_State = EVirtualMachineState_Running;

				co_return {};
			}

			TCFuture<void> f_Stop() override
			{
				using namespace NVirtualization;

				if (!mp_Launch)
				{
					mp_State = EVirtualMachineState_Stopped;
					co_return {};
				}

				mp_State = EVirtualMachineState_Stopping;

				// The termination signal asks the guest to shut down; the stop
				// escalates through the process launch when the guest does not
				co_await fg_TempCopy(mp_Launch)(&CProcessLaunchActor::f_StopProcess).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop the virtual machine window host")
				;

				co_await fg_Move(mp_Launch).f_Destroy().f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the virtual machine window host launch")
				;

				mp_LaunchSubscription.f_Clear();
				mp_State = EVirtualMachineState_Stopped;

				co_return {};
			}

			TCFuture<bool> f_WaitForStop(fp64 _TimeoutSeconds) override
			{
				using namespace NVirtualization;

				if (!mp_Launch || !mp_ExitFuture.f_IsValid())
					co_return true;

				// The window host exits by itself once its machine stops
				auto Result = co_await fg_Move(mp_ExitFuture)
					.f_Timeout(_TimeoutSeconds, "Timed out waiting for the virtual machine to stop")
					.f_Wrap()
				;

				co_return (bool)Result;
			}

			TCFuture<void> f_ForceStop() override
			{
				using namespace NVirtualization;

				if (!mp_Launch)
				{
					mp_State = EVirtualMachineState_Stopped;
					co_return {};
				}

				mp_State = EVirtualMachineState_Stopping;

				// Killing the host kills the machine with it
				co_await fg_TempCopy(mp_Launch)(&CProcessLaunchActor::f_Signal, int32(SIGKILL)).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to kill the virtual machine window host")
				;

				co_await fg_TempCopy(mp_Launch)(&CProcessLaunchActor::f_StopProcess).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop the virtual machine window host")
				;

				co_await fg_Move(mp_Launch).f_Destroy().f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the virtual machine window host launch")
				;

				mp_LaunchSubscription.f_Clear();
				mp_State = EVirtualMachineState_Stopped;

				co_return {};
			}

			TCFuture<NVirtualization::EVirtualMachineState> f_GetState() override
			{
				co_return mp_State;
			}

			TCFuture<NVirtualization::EVirtualMachineCapability> f_GetCapabilities() override
			{
				using namespace NVirtualization;

				co_return EVirtualMachineCapability_SharedFolders | EVirtualMachineCapability_MacOSGuests;
			}

		protected:
			TCFuture<void> fp_Destroy() override
			{
				if (mp_Launch)
				{
					co_await f_Stop().f_Wrap()
						> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop the virtual machine window host at destroy")
					;
				}

				co_return co_await CActor::fp_Destroy();
			}

		private:
			CStr mp_ConfigPath;
			CStr mp_WorkingDirectory;
			CStr mp_LogName;
			TCActor<NProcess::CProcessLaunchActor> mp_Launch;
			CActorSubscription mp_LaunchSubscription;
			TCFuture<void> mp_ExitFuture; /// Resolved when the window host process exits, which happens when its machine stops
			NVirtualization::EVirtualMachineState mp_State = NVirtualization::EVirtualMachineState_Stopped;
		};
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_VMSettings(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		bool bChanged = false;
		if (auto *pValue = _Params.f_GetMember("Window"))
		{
			bool bWindow = pValue->f_AsBoolean(false);
			if (bWindow != mp_bVMWindow)
			{
				mp_bVMWindow = bWindow;
				bChanged = true;
			}
		}

		co_await _pCommandLine->f_StdOut("Window: {}\n"_f << (mp_bVMWindow ? "true" : "false"));

		if (!bChanged)
			co_return 0;

		mp_State.m_StateDatabase.m_Data["VMSettings"]["Window"] = mp_bVMWindow;

		co_await mp_State.m_StateDatabase.f_Save();

		// Restart the running VM environments so the change takes effect
		TCVector<TCSharedPointer<CEnvironment>> RestartEnvironments;
		for (auto &pEnvironment : mp_Environments)
		{
			if (pEnvironment->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_VM)
				continue;

			if (!pEnvironment->f_IsStarted() && !pEnvironment->m_bStarting)
				continue;

			RestartEnvironments.f_Insert(fg_TempCopy(pEnvironment));
		}

		for (auto &pEnvironment : RestartEnvironments)
		{
			co_await _pCommandLine->f_StdOut("Restarting environment '{}'\n"_f << pEnvironment->m_Name);

			co_await fp_StopEnvironmentInternal(pEnvironment);
			co_await fp_EnsureEnvironmentStarted(pEnvironment);
		}

		if (!RestartEnvironments.f_IsEmpty())
		{
			// Relaunch the applications the environment stops stopped with the auto
			// start flag
			fp_UpdateApplicationDependencies();
		}

		co_return 0;
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

		_pEnvironment->f_SetStatus("Starting VM", CAppManagerInterface::EStatusSeverity_Warning);

		// The guest agent connects to the listen address on the shared network host side
		NWeb::NHTTP::CURL AgentAddress = co_await (fp_EnsureEnvironmentListen(_pEnvironment) % "Failed to add environment listen");

		// The agent files and deployment identity are placed inside the shared
		// storage, where the guest reaches them through the mounted share
		TCSharedPointer<CStr> pError = fg_Construct();
		CStr AgentExecutable = co_await fp_GetEnvironmentAgentExecutable(_pEnvironment, pError);
		if (AgentExecutable.f_IsEmpty())
		{
			_pEnvironment->f_SetStatus(*pError, CAppManagerInterface::EStatusSeverity_Error);
			co_return DMibErrorInstance(*pError);
		}

		_pEnvironment->m_LaunchID = fg_RandomID();

		// A fresh connection ticket lets the guest agent connect without the
		// standard stream handshake container agents use; the connect settings are
		// pushed to the agent directory on the guest disk through SSH
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

			CEJsonSorted ConnectJson;
			ConnectJson["Address"] = AgentAddress.f_Encode();
			ConnectJson["LaunchID"] = _pEnvironment->m_LaunchID;
			ConnectJson["Ticket"] = CStr(Ticket.m_Ticket.f_ToStringTicket());

			ConnectSettings = ConnectJson.f_ToString();
		}

		// A stable MAC address lets the host find the guest address in the DHCP
		// leases for the SSH agent setup
		if (_pEnvironment->m_VMMACAddress.f_IsEmpty())
		{
			_pEnvironment->m_VMMACAddress = fg_GenerateVMMACAddress();

			co_await fp_UpdateEnvironmentJson(_pEnvironment).f_Wrap()
				> fg_LogError("Malterlib/Cloud/AppManager", "Failed to save the environment MAC address")
			;
		}

		CVirtualMachineConfig Config = fp_BuildEnvironmentVMConfig(*_pEnvironment);
		Config.m_MACAddress = _pEnvironment->m_VMMACAddress;

		// Guest provisioning stored with the image is applied by the guest on its
		// first boot after restore and ignored afterwards
		Config.m_Provisioning = co_await fp_LoadVMImageProvisioning(BundleDirectory);

		CMacOSGuestProvisioning Provisioning = Config.m_Provisioning;

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
			// The window process hosts the machine in the graphical session; the
			// configuration hands it everything this start would have used itself
			CStr ConfigPath = fp_GetEnvironmentStorageDirectory(*_pEnvironment) / ".VMWindowHost.json";
			CStr ConfigContents = fg_AppManager_BuildVMWindowConfig
				(
					Config
					, Settings.m_VMBackend
					, fg_Format("AppManager Environment '{}'", _pEnvironment->m_Name)
				)
				.f_ToString()
			;

			{
				auto BlockingActorCheckout = fg_BlockingActor();

				co_await
					(
						g_Dispatch(BlockingActorCheckout) / [ConfigPath, ConfigContents]()
						{
							CFile::fs_WriteStringToFile(ConfigPath, ConfigContents);
						}
						% "Failed to write the window host configuration"
					)
				;
			}

			_pEnvironment->m_VMActor = fg_ConstructActor<CVMWindowHostActor>
				(
					fg_Move(ConfigPath)
					, CStr(mp_State.m_RootDirectory)
					, "Environment/{}/VMWindow"_f << _pEnvironment->m_Name
				)
			;
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
			fp_StopEnvironmentInternal(_pEnvironment).f_DiscardResult();
			co_return StartResult.f_GetException();
		}

		_pEnvironment->f_SetStatus("VM running, setting up the agent", CAppManagerInterface::EStatusSeverity_Warning);

		// The agent runs from the guest disk with per boot connect settings, so
		// every start pushes the agent files and the fresh settings through SSH and
		// restarts the agent daemon in the guest before waiting for it to connect
		if (!_pEnvironment->f_IsStarted())
		{
			bool bCanBootstrap = (bool)Provisioning && Provisioning.m_bEnableRemoteLogin;

			if (!bCanBootstrap)
			{
				CStr Error = "Cannot set up the agent for environment '{}': the VM image carries no provisioning credentials with remote login"_f << _pEnvironment->m_Name;
				_pEnvironment->f_SetStatus(Error, CAppManagerInterface::EStatusSeverity_Error);
				fp_StopEnvironmentInternal(_pEnvironment).f_DiscardResult();
				co_return DMibErrorInstance(Error);
			}

			auto BootstrapResult = co_await fp_BootstrapVMAgentOverSSH(_pEnvironment, Provisioning, AgentExecutable, ConnectSettings).f_Wrap();

			if (!BootstrapResult && !_pEnvironment->f_IsStarted())
			{
				CStr Error = fg_Format("Failed to set up the environment agent through SSH: {}", BootstrapResult.f_GetExceptionStr());
				_pEnvironment->f_SetStatus(Error, CAppManagerInterface::EStatusSeverity_Error);
				fp_StopEnvironmentInternal(_pEnvironment).f_DiscardResult();
				co_return BootstrapResult.f_GetException();
			}

			if (!_pEnvironment->f_IsStarted())
			{
				_pEnvironment->f_SetStatus("Agent installed, waiting for agent", CAppManagerInterface::EStatusSeverity_Warning);

				auto ConnectedResult = co_await _pEnvironment->m_OnAgentConnected.f_Insert().f_Future()
					.f_Timeout(120.0, "Timed out waiting for the environment agent in the VM to connect")
					.f_Wrap()
				;

				if (!ConnectedResult && !_pEnvironment->f_IsStarted())
				{
					// The guest side of the failure is in the agent log inside the
					// guest; fetch it so the log tells the whole story
					CStr GuestLog = co_await fp_FetchVMAgentGuestLog(_pEnvironment, Provisioning);

					DMibLogWithCategory
						(
							Malterlib/Cloud/AppManager
							, Error
							, "The agent for environment '{}' did not connect; guest agent log:\n{}"
							, _pEnvironment->m_Name
							, GuestLog
						)
					;

					_pEnvironment->f_SetStatus(fg_Format("Agent failed to connect: {}", ConnectedResult.f_GetExceptionStr()), CAppManagerInterface::EStatusSeverity_Error);
					fp_StopEnvironmentInternal(_pEnvironment).f_DiscardResult();
					co_return ConnectedResult.f_GetException();
				}
			}
		}

		co_return {};
	}

	namespace
	{
		// Captures the SSH output so failures can be reported with it
		struct CVMBootstrapLaunchActor : public NProcess::CProcessLaunchActor
		{
			struct CState
			{
				NThread::CMutual m_Lock;
				CStr m_Output;
			};

			CVMBootstrapLaunchActor(NStorage::TCSharedPointer<CState> const &_pState)
				: mp_pState(_pState)
			{
			}

		protected:
			bool fp_WillFilterOutput() override
			{
				return true;
			}

			void fp_FilterOutput(NProcess::EProcessLaunchOutputType _OutputType, CStr &o_Output) override
			{
				DMibLock(mp_pState->m_Lock);
				mp_pState->m_Output += o_Output;
			}

		private:
			NStorage::TCSharedPointer<CState> mp_pState;
		};

		struct CVMBootstrapRunResult
		{
			uint32 m_ExitCode = 0;
			CStr m_Output;
		};

		// The agent runs from the guest disk so it works as root independently of
		// the shared folders, which only carry the application directories
		constexpr ch8 const gc_pVMAgentGuestDirectory[] = "/opt/Malterlib/Agent";
		constexpr ch8 const gc_pVMAgentDaemonName[] = "MalterlibAppManagerAgent";

		TCVector<CStr> fg_CreateVMBootstrapSSHOptions()
		{
			return fg_CreateVector<CStr>
				(
					"-o", "StrictHostKeyChecking=no"
					, "-o", "UserKnownHostsFile=/dev/null"
					, "-o", "GlobalKnownHostsFile=/dev/null"
					, "-o", "ConnectTimeout=10"
					, "-o", "NumberOfPasswordPrompts=1"
					, "-o", "ServerAliveInterval=15"
					, "-o", "ServerAliveCountMax=4"
					, "-o", "LogLevel=ERROR"
				)
			;
		}

		// Runs one command against the guest (SSH itself, or a local shell piping
		// into SSH), authenticating through the ask pass helper and feeding the
		// input to the remote command once the process has launched: input sent
		// before the launch is silently dropped
		TCFuture<CVMBootstrapRunResult> fg_RunVMBootstrapSSH
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
			)
		{
			NStorage::TCSharedPointer<CVMBootstrapLaunchActor::CState> pOutputState = fg_Construct();

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

								auto Exception = DMibErrorInstance("Failed to launch SSH: {}"_f << LaunchError).f_ExceptionPointer();
								if (!Launched.f_IsSet())
									Launched.f_SetException(Exception);
								if (!Exited.f_IsSet())
									Exited.f_SetException(Exception);
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

			TCActor<NProcess::CProcessLaunchActor> pLaunchActor = fg_ConstructActor<CVMBootstrapLaunchActor>(pOutputState);

			auto LaunchSubscription = co_await fg_TempCopy(pLaunchActor)(&CProcessLaunchActor::f_Launch, Launch, _CallbackActor).f_Wrap();
			if (!LaunchSubscription)
			{
				co_await fg_Move(pLaunchActor).f_Destroy().f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the agent setup launch")
				;

				co_return LaunchSubscription.f_GetException();
			}

			auto LaunchedResult = co_await fg_Move(LaunchedPromise.m_Future)
				.f_Timeout(30.0, "Timed out waiting for SSH to launch")
				.f_Wrap()
			;

			if (LaunchedResult)
			{
				co_await fg_TempCopy(pLaunchActor)(&CProcessLaunchActor::f_SendStdIn, fg_Move(_StdInData)).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to send the agent setup input")
				;
				co_await fg_TempCopy(pLaunchActor)(&CProcessLaunchActor::f_CloseStdIn).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to close the agent setup input")
				;
			}

			auto ExitResult = co_await fg_Move(ExitPromise.m_Future)
				.f_Timeout(_Timeout, "The SSH agent setup attempt timed out")
				.f_Wrap()
			;

			co_await fg_Move(pLaunchActor).f_Destroy().f_Wrap()
				> fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the agent setup launch")
			;

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

	TCFuture<void> CAppManagerActor::fp_BootstrapVMAgentOverSSH
		(
			TCSharedPointer<CEnvironment> _pEnvironment
			, NVirtualization::CMacOSGuestProvisioning _Provisioning
			, CStr _AgentExecutable
			, CStr _ConnectSettings
		)
	{
		CStr MACAddress = _pEnvironment->m_VMMACAddress;
		CStr StorageDirectory = fp_GetEnvironmentStorageDirectory(*_pEnvironment);

		// The ask pass helper lets ssh take the password from the process
		// environment instead of a terminal prompt
		CStr AskPassPath = StorageDirectory / ".AgentBootstrapAskPass.sh";
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					g_Dispatch(BlockingActorCheckout) / [AskPassPath]()
					{
						CFile::fs_WriteStringToFile
							(
								AskPassPath
								, "#!/bin/sh\nprintf '%s\\n' \"$MALTERLIB_VM_SSH_PASSWORD\"\n"
								, false
								, EFileAttrib_Executable
							)
						;
					}
					% "Failed to write the SSH ask pass helper"
				)
			;
		}

		// The agent runs as root from the guest disk so it can create the per
		// application users and mount their shared folders. The daemon itself is
		// installed with the agent's own --daemon-add command; the connect
		// settings arrive on standard input behind the sudo password so the
		// ticket never touches a world readable location
		CStr AgentDirectory = CFile::fs_GetPath(_AgentExecutable);
		CStr ExecutableName = CFile::fs_GetFile(_AgentExecutable);

		// The agent files staged in the environment storage stream into the guest
		// as a tar archive; only the files that exist on the host are included
		TCVector<CStr> AgentFiles;
		{
			TCVector<CStr> Candidates = fg_CreateVector<CStr>
				(
					ExecutableName
					, "MalterlibHelper"
					, CFile::fs_GetFileNoExt(ExecutableName) + "VersionInfo.json"
					, "AppManagerSettings.json"
				)
			;

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

		CStr BootstrapScript = fg_Format
			(
				"set -e\n"
				"AgentDir='{}'\n"
				"if [ -x \"$AgentDir/{}\" ]; then\n"
				"\t\"$AgentDir/{}\" --daemon-stop --mode global '{}' >/dev/null 2>&1 || true\n"
				"fi\n"
				"launchctl bootout system/com.malterlib.appmanager.agent 2>/dev/null || true\n"
				"rm -f /Library/LaunchDaemons/com.malterlib.appmanager.agent.plist /usr/local/libexec/malterlib-appmanager-agent.sh /usr/local/libexec/malterlib-appmanager-agent-user.sh\n"
				"mkdir -p \"$AgentDir\"\n"
				"tar -xf /tmp/malterlib-agent-files.tar -C \"$AgentDir\"\n"
				"rm -f /tmp/malterlib-agent-files.tar\n"
				"umask 077\n"
				"cat > \"$AgentDir/AppManagerAgentConnect.json\"\n"
				"cd \"$AgentDir\"\n"
				"\"./{}\" --daemon-add --mode global --run-as-user root --run-as-group wheel --no-fail-if-added '{}'\n"
				, gc_pVMAgentGuestDirectory
				, ExecutableName
				, ExecutableName
				, gc_pVMAgentDaemonName
				, ExecutableName
				, gc_pVMAgentDaemonName
			)
		;

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "Setting up the agent for environment '{}' through SSH as user '{}', looking for MAC address {} in the DHCP leases"
				, _pEnvironment->m_Name
				, _Provisioning.m_Username
				, MACAddress
			)
		;

		// The guest needs time to finish its provisioning first boot before SSH
		// accepts connections, so the setup is retried until it succeeds
		CStr LastError = "The guest never appeared in the DHCP leases";
		CStr LastGuestIP;
		for (umint Attempt = 0; Attempt < 90; ++Attempt)
		{
			if (Attempt)
				co_await fg_Timeout(5.0);

			if (_pEnvironment->f_IsStarted())
				co_return {};

			if (_pEnvironment->m_bDeleted || _pEnvironment->m_bStopping)
				co_return DMibErrorInstance("Environment is stopping");

			CStr GuestIP;
			{
				auto BlockingActorCheckout = fg_BlockingActor();

				GuestIP = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [MACAddress]()
						{
							return fg_FindVMDHCPLease(MACAddress);
						}
					)
				;
			}

			if (GuestIP.f_IsEmpty())
				continue;

			if (GuestIP != LastGuestIP)
			{
				LastGuestIP = GuestIP;

				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Info
						, "Found the guest for environment '{}' at {}; installing the agent through SSH"
						, _pEnvironment->m_Name
						, GuestIP
					)
				;
			}

			CStr LogName = "Environment/{}/AgentBootstrap"_f << _pEnvironment->m_Name;
			CStr Target = fg_Format("{}@{}", _Provisioning.m_Username, GuestIP);

			TCVector<CStr> CommonOptions = fg_CreateVMBootstrapSSHOptions();

			// The agent files stream into the guest as a tar archive first, then the
			// setup script is staged with a plain cat, and finally the script runs
			// with the sudo password followed by the connect settings on standard
			// input, keeping the password and the ticket off the guest filesystem
			CStr Error;
			{
				CStr Pipeline = "tar -cf - -C '{}'"_f << AgentDirectory;
				for (auto &File : AgentFiles)
					Pipeline += " '{}'"_f << File;

				Pipeline += " | exec /usr/bin/ssh";
				for (auto &Option : CommonOptions)
					Pipeline += " '{}'"_f << Option;
				Pipeline += " '{}' 'cat > /tmp/malterlib-agent-files.tar'"_f << Target;

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
				Arguments.f_Insert(Target);
				for (auto pWord : {"cat", ">", "/tmp/malterlib-agent-bootstrap.sh"})
					Arguments.f_Insert(pWord);

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
				Arguments.f_Insert(Target);
				for (auto pWord : {"sudo", "-S", "-p", "''", "/bin/sh", "/tmp/malterlib-agent-bootstrap.sh", ";", "MalterlibStatus=$?", ";", "rm", "-f", "/tmp/malterlib-agent-bootstrap.sh", ";", "exit", "$MalterlibStatus"})
					Arguments.f_Insert(pWord);

				CStrIO PasswordInput;
				PasswordInput += _Provisioning.m_Password;
				PasswordInput += "\n";
				PasswordInput += _ConnectSettings;

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
					)
					.f_Wrap()
				;

				if (!RunResult)
					Error = RunResult.f_GetExceptionStr();
				else if (RunResult->m_ExitCode != 0)
					Error = fg_Format("SSH exited with status {}: {}", RunResult->m_ExitCode, CStr(RunResult->m_Output.f_Trim()));
				else
				{
					DMibLogWithCategory
						(
							Malterlib/Cloud/AppManager
							, Info
							, "Installed the agent for environment '{}' in the guest at {}"
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

				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Info
						, "Agent setup attempt for environment '{}' failed, retrying: {}"
						, _pEnvironment->m_Name
						, Error
					)
				;

				// Surface the current failure in the environment status so the setup
				// progress is visible without the log
				CStr StatusError = Error;
				if (auto iNewLine = StatusError.f_FindChar('\n'); iNewLine >= 0)
					StatusError = StatusError.f_Left(iNewLine);
				StatusError = StatusError.f_Left(160);

				_pEnvironment->f_SetStatus
					(
						"Setting up the environment agent through SSH: {}"_f << StatusError
						, CAppManagerInterface::EStatusSeverity_Warning
					)
				;
			}
		}

		co_return DMibErrorInstance("Failed to set up the agent in the guest: {}"_f << LastError);
	}

	TCFuture<CStr> CAppManagerActor::fp_FetchVMAgentGuestLog(TCSharedPointer<CEnvironment> _pEnvironment, NVirtualization::CMacOSGuestProvisioning _Provisioning)
	{
		CStr MACAddress = _pEnvironment->m_VMMACAddress;

		CStr GuestIP;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			GuestIP = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [MACAddress]()
					{
						return fg_FindVMDHCPLease(MACAddress);
					}
				)
			;
		}

		if (GuestIP.f_IsEmpty())
			co_return CStr("The guest was not found in the DHCP leases");

		// The agent daemon logs into its root directory on the guest disk, readable
		// only by root
		TCVector<CStr> Arguments = fg_CreateVMBootstrapSSHOptions();
		Arguments.f_Insert(fg_Format("{}@{}", _Provisioning.m_Username, GuestIP));
		for (auto pWord : {"sudo", "-S", "-p", "''", "tail", "-n", "60"})
			Arguments.f_Insert(pWord);
		Arguments.f_Insert(fg_Format("{}/Log/AppManager.log", gc_pVMAgentGuestDirectory));
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
				, fp_GetEnvironmentStorageDirectory(*_pEnvironment) / ".AgentBootstrapAskPass.sh"
				, _Provisioning.m_Password
				, "/usr/bin/ssh"
				, fg_Move(Arguments)
				, fg_Move(PasswordInput)
				, 30.0
			)
			.f_Wrap()
		;

		if (!Result)
			co_return CStr(fg_Format("Failed to fetch the guest agent log: {}", Result.f_GetExceptionStr()));

		co_return fg_Move(Result->m_Output);
	}

#ifdef DPlatformFamily_macOS
	TCFuture<uint32> CAppManagerActor::fp_CommandLine_EnvironmentWindow(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		using namespace NVirtualization;

		CStr Name = _Params["Name"].f_String();

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

		CVirtualMachineConfig Config = fp_BuildEnvironmentVMConfig(*pEnvironment);

		bool bBundleExists;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			CStr BundleDirectory = Config.m_BundleDirectory;
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
			co_await _pCommandLine->f_StdErr("VM image bundle '{}' does not exist\n"_f << Config.m_BundleDirectory);
			co_return 1;
		}

		// The window session hosts the virtual machine in the command line client
		// process instead of the AppManager, so the environment is stopped first to
		// release the guest image bundle
		bool bWasStarted = pEnvironment->f_IsStarted() || pEnvironment->m_bStarting;
		if (bWasStarted)
		{
			co_await _pCommandLine->f_StdOut("Stopping environment '{}'\n"_f << Name);

			auto StopResult = co_await fp_StopEnvironmentInternal(pEnvironment).f_Wrap();
			if (!StopResult)
				co_return _pCommandLine->f_AddAsyncResult(StopResult);
		}

		// The guest agent connects to the listen address on the shared network host
		// side, so an agent installed through the window can register right away
		co_await (fp_EnsureEnvironmentListen(pEnvironment) % "Failed to add environment listen");

		pEnvironment->f_SetStatus("Environment window open", CAppManagerInterface::EStatusSeverity_Warning);

		Config.m_Provisioning = co_await fp_LoadVMImageProvisioning(Config.m_BundleDirectory);
		Config.m_MACAddress = pEnvironment->m_VMMACAddress;

		CEJsonSorted ActionParams = fg_AppManager_BuildVMWindowConfig
			(
				Config
				, pEnvironment->m_Settings.m_VMBackend
				, fg_Format("AppManager Environment '{}'", Name)
			)
		;

		co_await _pCommandLine->f_StdOut
			(
				"Opening a window for environment '{}'\n"
				"Closing the window asks the guest to shut down; closing it again forces the stop\n"_f << Name
			)
		;

		auto WindowResult = co_await _pCommandLine->f_RunClientAction("VirtualMachineWindow", fg_Move(ActionParams)).f_Wrap();

		// The environment is restored no matter how the window session ended, so a
		// closed terminal or a failed window does not leave the environment stopped
		if (bWasStarted)
		{
			auto StartResult = co_await fp_EnsureEnvironmentStarted(pEnvironment).f_Wrap();
			if (StartResult)
			{
				// Relaunch the applications the environment stop stopped with the
				// auto start flag
				fp_UpdateApplicationDependencies();
			}
			else if (WindowResult)
				co_return _pCommandLine->f_AddAsyncResult(StartResult);
			else
				(void)_pCommandLine->f_AddAsyncResult(StartResult);
		}
		else
			pEnvironment->f_SetStatus("Not started", CAppManagerInterface::EStatusSeverity_Warning);

		co_return _pCommandLine->f_AddAsyncResult(WindowResult);
	}
#endif

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

		// Guest provisioning is stored with the image and applied by whoever boots
		// it first: the first environment start or environment window
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
					g_Dispatch(BlockingActorCheckout) / [ProvisioningPath, Contents]()
					{
						CFile::fs_WriteStringToFile(ProvisioningPath, Contents);
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
