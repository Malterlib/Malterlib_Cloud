// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud
{
	namespace
	{
		using namespace NMib::NStr;
		using namespace NMib::NContainer;

		CStr fg_UnquoteOSReleaseValue(CStr const &_Value)
		{
			CStr Value = _Value.f_Trim();
			if (Value.f_GetLen() >= 2 && (Value.f_StartsWith("\"") || Value.f_StartsWith("'")) && Value.f_Right(1) == Value.f_Left(1))
				Value = Value.f_Extract(1).f_Left(Value.f_GetLen() - 2);

			return Value;
		}

		bool fg_IsValidPackageName(CStr const &_Package)
		{
			if (_Package.f_IsEmpty())
				return false;

			for (auto Char : _Package)
			{
				if (Char >= 'a' && Char <= 'z')
					continue;
				if (Char >= 'A' && Char <= 'Z')
					continue;
				if (Char >= '0' && Char <= '9')
					continue;
				if (Char == '@' || Char == '.' || Char == '_' || Char == '+' || Char == '-' || Char == ':')
					continue;

				return false;
			}

			return true;
		}
	}

	CAppManagerOSIdentity fg_AppManager_ParseOSRelease(NStr::CStr const &_Contents)
	{
		using namespace NMib::NStr;

		CAppManagerOSIdentity Identity;

		for (auto &Line : _Contents.f_Split("\n"))
		{
			auto iEquals = Line.f_Find("=");
			if (iEquals < 0)
				continue;

			CStr Key = Line.f_Left(iEquals).f_Trim();
			CStr Value = fg_UnquoteOSReleaseValue(Line.f_Extract(iEquals + 1));

			if (Key == "ID")
				Identity.m_ID = Value.f_LowerCase();
			else if (Key == "ID_LIKE")
			{
				for (auto &Like : Value.f_Split(" "))
				{
					if (!Like.f_IsEmpty())
						Identity.m_Like.f_Insert(Like.f_LowerCase());
				}
			}
		}

		return Identity;
	}

	CAppManagerOSIdentity fg_AppManager_GetOSIdentity()
	{
		using namespace NMib::NStr;

#if defined(DPlatformFamily_macOS)
		return {.m_ID = "macos"};
#elif defined(DPlatformFamily_Windows)
		return {.m_ID = "windows"};
#else
		CStr Contents;

		try
		{
			Contents = NFile::CFile::fs_ReadStringFromFile(CStr("/etc/os-release"));
		}
		catch (...)
		{
		}

		auto Identity = fg_AppManager_ParseOSRelease(Contents);
		if (Identity.m_ID.f_IsEmpty())
			Identity.m_ID = "linux";

		return Identity;
#endif
	}

	NStr::CStr fg_AppManager_BuildOSDependencyInstallScript
		(
			CAppManagerOSIdentity const &_Identity
			, NContainer::TCMap<NStr::CStr, NContainer::TCVector<NStr::CStr>> const &_Dependencies
			, NStr::CStr &o_Error
		)
	{
		using namespace NMib::NStr;
		using namespace NMib::NContainer;

		o_Error = {};

		bool bLinux = _Identity.m_ID != "macos" && _Identity.m_ID != "windows";

		auto fMatchesIdentity = [&](CStr const &_Selector)
			{
				CStr Selector = _Selector.f_LowerCase();
				if (Selector == _Identity.m_ID)
					return true;

				if (bLinux && Selector == "linux")
					return true;

				for (auto &Like : _Identity.m_Like)
				{
					if (Selector == Like)
						return true;
				}

				return false;
			}
		;

		TCSet<CStr> Packages;
		for (auto &SelectorPackages : _Dependencies)
		{
			if (!fMatchesIdentity(_Dependencies.fs_GetKey(SelectorPackages)))
				continue;

			for (auto &Package : SelectorPackages)
				Packages[Package];
		}

		if (Packages.f_IsEmpty())
			return {};

		CStr PackageList;
		for (auto &Package : Packages)
		{
			if (!fg_IsValidPackageName(Package))
			{
				o_Error = fg_Format("'{}' is not a valid package name", Package);
				return {};
			}

			if (!PackageList.f_IsEmpty())
				PackageList += " ";
			PackageList += Package;
		}

		auto fUsesManager = [&](CStr const &_ID)
			{
				if (_Identity.m_ID == _ID)
					return true;

				for (auto &Like : _Identity.m_Like)
				{
					if (Like == _ID)
						return true;
				}

				return false;
			}
		;

		if (fUsesManager("debian") || fUsesManager("ubuntu"))
		{
			return fg_Format
				(
					"set -e\n"
					"export DEBIAN_FRONTEND=noninteractive\n"
					"apt-get update -qq\n"
					"apt-get install -y -qq --no-install-recommends {}\n"
					, PackageList
				)
			;
		}

		if (fUsesManager("fedora") || fUsesManager("rhel") || fUsesManager("centos"))
			return fg_Format("set -e\ndnf install -y {}\n", PackageList);

		if (fUsesManager("alpine"))
			return fg_Format("set -e\napk add {}\n", PackageList);

		if (fUsesManager("arch"))
			return fg_Format("set -e\npacman -Sy --noconfirm --needed {}\n", PackageList);

		if (fUsesManager("suse") || fUsesManager("opensuse") || fUsesManager("opensuse-leap") || fUsesManager("opensuse-tumbleweed") || fUsesManager("sles"))
			return fg_Format("set -e\nzypper --non-interactive install {}\n", PackageList);

		if (_Identity.m_ID == "macos")
		{
			// Homebrew refuses to run as root, so it runs as the owner of the brew
			// executable when the AppManager runs as a root daemon
			return fg_Format
				(
					"set -e\n"
					"BrewExecutable=\"$(command -v brew || true)\"\n"
					"if [ -z \"$BrewExecutable\" ]; then\n"
					"	for Candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do\n"
					"		if [ -x \"$Candidate\" ]; then\n"
					"			BrewExecutable=\"$Candidate\"\n"
					"			break\n"
					"		fi\n"
					"	done\n"
					"fi\n"
					"if [ -z \"$BrewExecutable\" ]; then\n"
					"	echo \"Homebrew is not installed\" >&2\n"
					"	exit 1\n"
					"fi\n"
					"if [ \"$(id -u)\" = \"0\" ]; then\n"
					"	BrewUser=\"$(stat -f %Su \"$BrewExecutable\")\"\n"
					"	sudo -u \"$BrewUser\" -H \"$BrewExecutable\" install {}\n"
					"else\n"
					"	\"$BrewExecutable\" install {}\n"
					"fi\n"
					, PackageList
					, PackageList
				)
			;
		}

		o_Error = fg_Format("No supported package manager for operating system '{}'", _Identity.m_ID);
		return {};
	}
}

namespace NMib::NCloud::NAppManager
{
	CStr CAppManagerActor::fsp_GetOSDependenciesFingerprint(TCMap<CStr, TCVector<CStr>> const &_Dependencies)
	{
		CStr Fingerprint;
		for (auto &Packages : _Dependencies)
		{
			Fingerprint += _Dependencies.fs_GetKey(Packages) + ":";
			for (auto &Package : Packages)
				Fingerprint += " " + Package;
			Fingerprint += "\n";
		}

		return Fingerprint;
	}

	TCFuture<void> CAppManagerActor::fp_InstallOSDependencies(TCMap<CStr, TCVector<CStr>> _Dependencies)
	{
		CAppManagerOSIdentity Identity;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			Identity = co_await
				(
					g_Dispatch(BlockingActorCheckout) / []()
					{
						return fg_AppManager_GetOSIdentity();
					}
				)
			;
		}

		CStr Error;
		CStr Script = fg_AppManager_BuildOSDependencyInstallScript(Identity, _Dependencies, Error);
		if (!Error.f_IsEmpty())
			co_return DMibErrorInstance(fg_Move(Error));

		if (Script.f_IsEmpty())
			co_return {};

		CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
			(
				"/bin/bash"
				, fg_CreateVector<CStr>("-ec", Script)
				, mp_State.m_RootDirectory
				, {}
			)
		;
		LaunchParams.m_bMergeEnvironment = true;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple
			(
				CProcessLaunchActor::CSimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode)
			)
			.f_Wrap()
		;

		if (!Result)
			co_return Result.f_GetException();

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_EnsureOSDependencies(TCSharedPointer<CApplication> _pApplication)
	{
		auto &Dependencies = _pApplication->m_Settings.m_OSDependencies;
		if (Dependencies.f_IsEmpty())
			co_return {};

		CStr Fingerprint = fsp_GetOSDependenciesFingerprint(Dependencies);

		// Installs run once per AppManager instance and dependency set; a recreated
		// container starts a fresh agent, so its reset OS is populated again
		if (Fingerprint == _pApplication->m_InstalledOSDependenciesFingerprint)
			co_return {};

		fp_SetAppLaunchStatus(_pApplication, "Installing OS dependencies", CAppManagerInterface::EStatusSeverity_Warning);

		auto Result = co_await fp_InstallOSDependencies(fg_TempCopy(Dependencies)).f_Wrap();
		if (!Result)
			co_return DMibErrorInstance("Failed to install OS dependencies for '{}': {}"_f << _pApplication->m_Name << Result.f_GetExceptionStr());

		_pApplication->m_InstalledOSDependenciesFingerprint = Fingerprint;

		co_return {};
	}
}
