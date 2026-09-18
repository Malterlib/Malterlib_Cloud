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
			// apt fails at once on a held lock by default, and the lock timeout doesn't cover the package list lock of apt-get update, which retries.
			// Installed packages that satisfy the names, also providers of virtual names, are marked manual where possible, so that autoremove keeps
			// them; a failed marking doesn't fail the installation. A name is installed when an installed package has it with its architecture, an
			// all package also with the native one, or without one for a native, all or not Multi-Arch: same package, or provides it with or without
			// the architecture of the provider. The second letter of the status is the installed state.
			// apt-get takes an unknown name as a pattern, so the missing names and their architectures are checked before they are installed
			return fg_Format
				(
					"set -e\n"
					"export DEBIAN_FRONTEND=noninteractive\n"
					"export NEEDRESTART_MODE=l\n"
					"Installed=\"$(dpkg-query -W -f='{}' | grep '^.i ' | cut -c4- | awk -v Native=\"$(dpkg --print-architecture)\" '{}')\"\n"
					"Missing=\n"
					"Satisfying=\n"
					"for Package in {}; do\n"
					"	Providers=\"$(printf '%s\\n' \"$Installed\" | awk -v Name=\"$Package\" '{}')\"\n"
					"	if [ -z \"$Providers\" ]; then\n"
					"		Missing=\"$Missing $Package\"\n"
					"	else\n"
					"		Satisfying=\"$Satisfying $Providers\"\n"
					"	fi\n"
					"done\n"
					"Automatic=\n"
					"if [ -n \"$Satisfying\" ]; then\n"
					"	Automatic=\"$(apt-mark showauto $Satisfying 2>/dev/null || true)\"\n"
					"fi\n"
					"if [ -n \"$Automatic\" ] && ! apt-mark manual $Automatic >/dev/null; then\n"
					"	echo \"Failed to mark $Automatic as installed manually, so apt-get autoremove can remove them\" >&2\n"
					"fi\n"
					"if [ -z \"$Missing\" ]; then\n"
					"	exit 0\n"
					"fi\n"
					"Attempt=0\n"
					"while ! Output=\"$(apt-get -o DPkg::Lock::Timeout=600 update -qq 2>&1)\"; do\n"
					"	Attempt=$((Attempt + 1))\n"
					"	case \"$Output\" in\n"
					"		*/var/lib/apt/lists/lock*) ;;\n"
					"		*) printf '%s\\n' \"$Output\" >&2; exit 1;;\n"
					"	esac\n"
					"	if [ $Attempt -ge 60 ]; then\n"
					"		printf '%s\\n' \"$Output\" >&2\n"
					"		exit 1\n"
					"	fi\n"
					"	sleep 10\n"
					"done\n"
					"Architectures=\"$(dpkg --print-architecture; dpkg --print-foreign-architectures)\"\n"
					"for Package in $Missing; do\n"
					"	Name=\"$(printf '%s\\n' \"$Package\" | cut -d: -f1)\"\n"
					"	Architecture=\"$(printf '%s\\n' \"$Package\" | cut -s -d: -f2-)\"\n"
					"	if [ \"$Name\" != \"$Package\" ] && ! printf '%s\\n' all $Architectures | grep -Fxq -- \"$Architecture\"; then\n"
					"		echo \"dpkg has no architecture named $Architecture for $Name\" >&2\n"
					"		exit 1\n"
					"	fi\n"
					"	if ! apt-cache --all-names pkgnames \"$Name\" | grep -Fxq -- \"$Name\"; then\n"
					"		echo \"No package or virtual package is named $Name\" >&2\n"
					"		exit 1\n"
					"	fi\n"
					"done\n"
					// An upgrade of a package whose configuration file was edited locally asks dpkg what to keep, which a noninteractive front end
					// doesn't answer; the local edits are kept
					"apt-get -o DPkg::Lock::Timeout=600 -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold install -y -qq --no-install-recommends "
					"--no-remove $Missing\n"
					, "${db:Status-Abbrev}${Architecture}|${Multi-Arch}|${Package}|${Provides}\\n"
					, "BEGIN { FS = \"|\" } {"
					" Package = ($1 == Native || $1 == \"all\") ? $3 : ($3 \":\" $1);"
					" print $3 \":\" $1 \" \" Package;"
					" if ($1 == \"all\") print $3 \":\" Native \" \" Package;"
					" if ($1 == Native || $1 == \"all\" || $2 != \"same\") print $3 \" \" Package;"
					" Count = split($4, Provides, \",\");"
					" for (i = 1; i <= Count; ++i) {"
					" Name = Provides[i]; sub(/\\(.*/, \"\", Name); gsub(/ /, \"\", Name);"
					" if (Name != \"\") { print Name \" \" Package; print Name \":\" ($1 == \"all\" ? Native : $1) \" \" Package }"
					" }"
					" }"
					, PackageList
					, "$1 == Name { print $2 }"
				)
			;
		}

		// Minimal images ship microdnf instead of dnf
		if (fUsesManager("fedora") || fUsesManager("rhel") || fUsesManager("centos"))
		{
			return fg_Format
				(
					"set -e\n"
					"if rpm -q {} >/dev/null 2>&1; then\n"
					"	exit 0\n"
					"fi\n"
					"if command -v dnf >/dev/null 2>&1; then\n"
					"	dnf install -y {}\n"
					"else\n"
					"	microdnf install -y {}\n"
					"fi\n"
					, PackageList
					, PackageList
					, PackageList
				)
			;
		}

		// Images don't ship the repository indexes, which --no-cache fetches without keeping them
		if (fUsesManager("alpine"))
		{
			return fg_Format
				(
					"set -e\n"
					"if apk info -e {} >/dev/null 2>&1; then\n"
					"	exit 0\n"
					"fi\n"
					"apk add --no-cache {}\n"
					, PackageList
					, PackageList
				)
			;
		}

		if (fUsesManager("arch"))
		{
			// Refreshing the databases without a full upgrade is an unsupported partial upgrade on Arch
			return fg_Format
				(
					"set -e\n"
					"if pacman -Q {} >/dev/null 2>&1; then\n"
					"	exit 0\n"
					"fi\n"
					"pacman -Syu --noconfirm --needed {}\n"
					, PackageList
					, PackageList
				)
			;
		}

		if (fUsesManager("suse") || fUsesManager("opensuse") || fUsesManager("opensuse-leap") || fUsesManager("opensuse-tumbleweed") || fUsesManager("sles"))
		{
			return fg_Format
				(
					"set -e\n"
					"if rpm -q {} >/dev/null 2>&1; then\n"
					"	exit 0\n"
					"fi\n"
					"zypper --non-interactive install {}\n"
					, PackageList
					, PackageList
				)
			;
		}

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
