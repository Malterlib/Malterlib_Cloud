// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Concurrency/LogError>

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
			// A leading hyphen would make the name an option of the package manager, and a trailing one asks apt-get install to
			// remove the package
			if (_Package.f_IsEmpty() || _Package.f_StartsWith("/") || _Package.f_StartsWith("-") || _Package.f_EndsWith("-"))
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

				// Fully qualified Homebrew formulas name their tap
				if (Char == '/')
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

		// The os-release format names /usr/lib/os-release as the fallback when /etc/os-release is missing
		for (auto *pPath : {"/etc/os-release", "/usr/lib/os-release"})
		{
			try
			{
				Contents = NFile::CFile::fs_ReadStringFromFile(CStr(pPath));
				break;
			}
			catch (NException::CException const &)
			{
			}
		}

		auto Identity = fg_AppManager_ParseOSRelease(Contents);
		if (Identity.m_ID.f_IsEmpty())
			Identity.m_ID = "linux";

		return Identity;
#endif
	}

	// Returns an empty script when no dependency matches the identity; sets o_Error when matching dependencies cannot be installed
	auto fg_AppManager_BuildOSDependencyInstallScript
		(
			CAppManagerOSIdentity const &_Identity
			, NContainer::TCMap<NStr::CStr, NContainer::TCVector<NStr::CStr>> const &_Dependencies
			, NStr::CStr &o_Error
		)
		-> NStr::CStr
	{
		using namespace NMib::NStr;
		using namespace NMib::NContainer;

		o_Error = {};

		bool bLinux = _Identity.m_ID != "macos" && _Identity.m_ID != "windows";

		auto fMatchesIdentity = [&](CStr const &_Selector)
			{
				CStr Selector = _Selector.f_LowerCase();
				if (Selector == "*")
					return true;

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

		// The Linux package managers write their databases even when nothing is missing, which fails on a read-only root, so
		// installed packages are checked first
		if (fUsesManager("debian") || fUsesManager("ubuntu"))
		{
			// apt fails immediately on a held lock by default; automatic patching and unattended upgrades hold it for a while. The lock timeout
			// doesn't cover the package list lock that apt-get update takes, so the update retries while another one holds it. The packages
			// that satisfy the names and are already installed as dependencies of others are marked as installed manually, as the install
			// would, so that apt-get autoremove keeps them, also the providers of a virtual package. A name is installed when an installed
			// package has it with its architecture, or without it for a package of the native architecture, of all architectures or one that
			// isn't Multi-Arch: same, which apt installs again for the native architecture, or provides it, since apt refuses to install a
			// virtual package that several packages provide also when one of them is installed. Each line of the installed names is a name and
			// the package that satisfies it. The second letter of the status is the installed state; the first is the wanted state, which is
			// hold for a held package. apt-get takes a name it can't find, like one with a dot, as a pattern that can match other packages, so
			// the missing names must be names that apt knows exactly, and architectures that dpkg knows, before they are installed
			return fg_Format
				(
					"set -e\n"
					"export DEBIAN_FRONTEND=noninteractive\n"
					"export NEEDRESTART_MODE=l\n"
					"Installed=\"$(dpkg-query -W -f='{}' | grep '^.i ' | cut -c4- | awk -v Native=\"$(dpkg --print-architecture)\" '{}')\"\n"
					"Missing=\n"
					"Satisfying=\n"
					"for Package in {}; do\n"
					"\tProviders=\"$(printf '%s\\n' \"$Installed\" | awk -v Name=\"$Package\" '{}')\"\n"
					"\tif [ -z \"$Providers\" ]; then\n"
					"\t\tMissing=\"$Missing $Package\"\n"
					"\telse\n"
					"\t\tSatisfying=\"$Satisfying $Providers\"\n"
					"\tfi\n"
					"done\n"
					"Automatic=\n"
					"if [ -n \"$Satisfying\" ]; then\n"
					"\tAutomatic=\"$(apt-mark showauto $Satisfying 2>/dev/null || true)\"\n"
					"fi\n"
					"if [ -n \"$Automatic\" ] && ! apt-mark manual $Automatic >/dev/null; then\n"
					"\techo \"Failed to mark $Automatic as installed manually, so apt-get autoremove can remove them\" >&2\n"
					"fi\n"
					"if [ -z \"$Missing\" ]; then\n"
					"\texit 0\n"
					"fi\n"
					"Attempt=0\n"
					"while ! Output=\"$(apt-get -o DPkg::Lock::Timeout=600 update -qq 2>&1)\"; do\n"
					"\tAttempt=$((Attempt + 1))\n"
					"\tcase \"$Output\" in\n"
					"\t\t*/var/lib/apt/lists/lock*) ;;\n"
					"\t\t*) printf '%s\\n' \"$Output\" >&2; exit 1;;\n"
					"\tesac\n"
					"\tif [ $Attempt -ge 60 ]; then\n"
					"\t\tprintf '%s\\n' \"$Output\" >&2\n"
					"\t\texit 1\n"
					"\tfi\n"
					"\tsleep 10\n"
					"done\n"
					"Architectures=\"$(dpkg --print-architecture; dpkg --print-foreign-architectures)\"\n"
					"for Package in $Missing; do\n"
					"\tName=\"$(printf '%s\\n' \"$Package\" | cut -d: -f1)\"\n"
					"\tArchitecture=\"$(printf '%s\\n' \"$Package\" | cut -s -d: -f2-)\"\n"
					"\tif [ \"$Name\" != \"$Package\" ] && ! printf '%s\\n' all $Architectures | grep -Fxq -- \"$Architecture\"; then\n"
					"\t\techo \"dpkg has no architecture named $Architecture for $Name\" >&2\n"
					"\t\texit 1\n"
					"\tfi\n"
					"\tif ! apt-cache --all-names pkgnames \"$Name\" | grep -Fxq -- \"$Name\"; then\n"
					"\t\techo \"No package or virtual package is named $Name\" >&2\n"
					"\t\texit 1\n"
					"\tfi\n"
					"done\n"
					"apt-get -o DPkg::Lock::Timeout=600 install -y -qq --no-install-recommends --no-remove $Missing\n"
					, "${db:Status-Abbrev}${Architecture}|${Multi-Arch}|${Package}|${Provides}\\n"
					, "BEGIN { FS = \"|\" } {"
					" Package = ($1 == Native || $1 == \"all\") ? $3 : ($3 \":\" $1);"
					" print $3 \":\" $1 \" \" Package;"
					" if ($1 == Native || $1 == \"all\" || $2 != \"same\") print $3 \" \" Package;"
					" Count = split($4, Provides, \",\");"
					" for (i = 1; i <= Count; ++i) { Name = Provides[i]; sub(/\\(.*/, \"\", Name); gsub(/ /, \"\", Name); if (Name != \"\") print Name \" \" Package }"
					" }"
					, PackageList
					, "$1 == Name { print $2 }"
				)
			;
		}

		if (fUsesManager("fedora") || fUsesManager("rhel") || fUsesManager("centos"))
			return fg_Format("set -e\nif rpm -q {} >/dev/null 2>&1; then\n\texit 0\nfi\ndnf install -y {}\n", PackageList, PackageList);

		// Images don't ship the repository indexes, which --no-cache fetches without keeping them
		if (fUsesManager("alpine"))
			return fg_Format("set -e\nif apk info -e {} >/dev/null 2>&1; then\n\texit 0\nfi\napk add --no-cache {}\n", PackageList, PackageList);

		if (fUsesManager("arch"))
		{
			// Refreshing the databases without a full upgrade is an unsupported partial upgrade on Arch
			return fg_Format("set -e\nif pacman -Q {} >/dev/null 2>&1; then\n\texit 0\nfi\npacman -Syu --noconfirm --needed {}\n", PackageList, PackageList);
		}

		if (fUsesManager("suse") || fUsesManager("opensuse") || fUsesManager("opensuse-leap") || fUsesManager("opensuse-tumbleweed") || fUsesManager("sles"))
			return fg_Format("set -e\nif rpm -q {} >/dev/null 2>&1; then\n\texit 0\nfi\nzypper --non-interactive install {}\n", PackageList, PackageList);

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
					"	BrewUser=\"$(stat -L -f %Su \"$BrewExecutable\")\"\n"
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
		// The names aren't validated yet, so each is prefixed with its length to keep lists that join to the same text apart
		CStr Fingerprint;
		for (auto &Packages : _Dependencies)
		{
			auto &Selector = _Dependencies.fs_GetKey(Packages);
			Fingerprint += "{}:{}:"_f << Selector.f_GetLen() << Selector;
			for (auto &Package : Packages)
				Fingerprint += " {}:{}"_f << Package.f_GetLen() << Package;
			Fingerprint += "\n";
		}

		return Fingerprint;
	}

	TCFuture<void> CAppManagerActor::fp_InstallOSDependencies(TCMap<CStr, TCVector<CStr>> _Dependencies)
	{
		auto SequenceSubscription = co_await mp_OSDependenciesSequencer.f_Sequence();

		// An install of the same dependencies can finish while this one waits for its turn
		CStr Fingerprint = fsp_GetOSDependenciesFingerprint(_Dependencies);
		if (mp_InstalledOSDependenciesFingerprints.f_FindEqual(Fingerprint))
			co_return {};

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

		// Stopping the environment or rebooting the host in the middle of an installation leaves the packages broken, so an agent installs only
		// once the host has admitted the installation, which it refuses once it stops the environment or reboots. Without a connection to the
		// host nothing admits it
		if (mp_bEnvironmentAgent && !mp_EnvironmentHostActor)
			co_return DMibErrorInstance("Not connected to the environment host, which admits the installation");

		++mp_nEnvironmentOSDependencyInstalls;
		auto ReportFinished = g_OnScopeExit / [this]
			{
				--mp_nEnvironmentOSDependencyInstalls;
				if (mp_bEnvironmentAgent)
					fp_ReportEnvironmentBusyToHost() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to report the environment update state");
			}
		;

		// A failed report also ends with the report of the finished installation, since one that timed out can have reached the host
		if (mp_bEnvironmentAgent)
		{
			auto ReportResult = co_await fp_ReportEnvironmentBusyToHost().f_Wrap();
			if (!ReportResult)
				co_return DMibErrorInstance("The environment host did not admit the installation: {}"_f << ReportResult.f_GetExceptionStr());
		}

		CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable("/bin/sh", fg_CreateVector<CStr>("-ec", Script), mp_State.m_RootDirectory, {});
		LaunchParams.m_bMergeEnvironment = true;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple
			(
				CProcessLaunchActor::CSimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode)
			)
			.f_Wrap()
		;

		if (!Result)
			co_return Result.f_GetException();

		if (CStr Errors = Result->f_GetStdErr().f_Trim(); !Errors.f_IsEmpty())
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "The OS dependency installation reported: {}", Errors);

		mp_InstalledOSDependenciesFingerprints[Fingerprint];

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_EnsureOSDependencies(TCSharedPointer<CApplication> _pApplication)
	{
		auto &Dependencies = _pApplication->m_Settings.m_OSDependencies;
		if (Dependencies.f_IsEmpty())
			co_return {};

		CStr Fingerprint = fsp_GetOSDependenciesFingerprint(Dependencies);

		// The installed sets outlive the entries of applications, which an environment agent creates for each launch; a recreated container
		// starts a fresh agent, which installs into its reset OS again
		if (mp_InstalledOSDependenciesFingerprints.f_FindEqual(Fingerprint))
			co_return {};

		fp_SetAppLaunchStatus(_pApplication, "Installing OS dependencies", CAppManagerInterface::EStatusSeverity_Warning);

		auto Result = co_await fp_InstallOSDependencies(fg_TempCopy(Dependencies)).f_Wrap();
		if (!Result)
			co_return DMibErrorInstance("Failed to install OS dependencies for '{}': {}"_f << _pApplication->m_Name << Result.f_GetExceptionStr());

		co_return {};
	}
}
