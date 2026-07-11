// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Test_Malterlib_Cloud_AppManager.h"

struct CAppManager_OSDependencies_Tests : public NMib::NTest::CTest
{
	void f_DoTests();
};

void CAppManager_OSDependencies_Tests::f_DoTests()
{
	DMibTestSuite("Parse OS Release") -> TCFuture<void>
	{
		{
			DMibTestPath("Ubuntu");

			auto Identity = fg_AppManager_ParseOSRelease
				(
					"PRETTY_NAME=\"Ubuntu 24.04.1 LTS\"\n"
					"NAME=\"Ubuntu\"\n"
					"ID=ubuntu\n"
					"ID_LIKE=debian\n"
					"VERSION_ID=\"24.04\"\n"
				)
			;
			DMibExpect(Identity.m_ID, ==, "ubuntu");
			DMibExpect(Identity.m_Like.f_GetLen(), ==, 1u);
			DMibExpect(Identity.m_Like[0], ==, "debian");
		}

		{
			DMibTestPath("Quoted Multiple Like");

			auto Identity = fg_AppManager_ParseOSRelease
				(
					"ID=\"centos\"\n"
					"ID_LIKE=\"rhel fedora\"\n"
				)
			;
			DMibExpect(Identity.m_ID, ==, "centos");
			DMibExpect(Identity.m_Like.f_GetLen(), ==, 2u);
			DMibExpect(Identity.m_Like[0], ==, "rhel");
			DMibExpect(Identity.m_Like[1], ==, "fedora");
		}

		{
			DMibTestPath("Empty");

			auto Identity = fg_AppManager_ParseOSRelease("");
			DMibExpectTrue(Identity.m_ID.f_IsEmpty());
			DMibExpectTrue(Identity.m_Like.f_IsEmpty());
		}

		co_return {};
	};

	DMibTestSuite("Install Script") -> TCFuture<void>
	{
		TCMap<CStr, TCVector<CStr>> Dependencies;
		Dependencies["Ubuntu"] = fg_CreateVector<CStr>("libxml2", "curl");
		Dependencies["Linux"] = fg_CreateVector<CStr>("ca-certificates");
		Dependencies["macOS"] = fg_CreateVector<CStr>("openssl@3");

		{
			DMibTestPath("Ubuntu Apt");

			CStr Error;
			CStr Script = fg_AppManager_BuildOSDependencyInstallScript({.m_ID = "ubuntu", .m_Like = {"debian"}}, Dependencies, Error);
			DMibExpectTrue(Error.f_IsEmpty());
			DMibExpectTrue(Script.f_Find("for Package in ca-certificates curl libxml2; do") >= 0);
			DMibExpectTrue(Script.f_Find("dpkg-query -W -f='${db:Status-Abbrev}${Architecture}|${Multi-Arch}|${Package}|${Provides}\\n'") >= 0);
			DMibExpectTrue(Script.f_Find("apt-get -o DPkg::Lock::Timeout=600 update") >= 0);
			DMibExpectTrue(Script.f_Find("apt-get -o DPkg::Lock::Timeout=600 install -y -qq --no-install-recommends --no-remove $Missing") >= 0);
			DMibExpectTrue(Script.f_Find("apt-mark showauto $Satisfying") >= 0);
			DMibExpectTrue(Script.f_Find("*/var/lib/apt/lists/lock*") >= 0);

			// apt-get takes a name it can't find as a pattern, so the exact names are checked before the install
			auto iInstall = Script.f_Find("install -y -qq");
			DMibExpectTrue(iInstall >= 0);
			DMibExpect(Script.f_Left(iInstall).f_FindReverse("apt-cache --all-names pkgnames \"$Name\" | grep -Fxq -- \"$Name\""), >, Script.f_Find("update -qq"));
		}

		{
			DMibTestPath("Debian Via Like");

			CStr Error;
			TCMap<CStr, TCVector<CStr>> LikeDependencies;
			LikeDependencies["debian"] = fg_CreateVector<CStr>("curl");

			CStr Script = fg_AppManager_BuildOSDependencyInstallScript({.m_ID = "ubuntu", .m_Like = {"debian"}}, LikeDependencies, Error);
			DMibExpectTrue(Error.f_IsEmpty());
			DMibExpectTrue(Script.f_Find("for Package in curl; do") >= 0);
		}

		{
			DMibTestPath("Fedora Dnf");

			CStr Error;
			CStr Script = fg_AppManager_BuildOSDependencyInstallScript({.m_ID = "fedora"}, Dependencies, Error);
			DMibExpectTrue(Error.f_IsEmpty());
			DMibExpectTrue(Script.f_Find("dnf install -y ca-certificates") >= 0);
		}

		{
			DMibTestPath("Alpine Apk");

			CStr Error;
			CStr Script = fg_AppManager_BuildOSDependencyInstallScript({.m_ID = "alpine"}, Dependencies, Error);
			DMibExpectTrue(Error.f_IsEmpty());
			DMibExpectTrue(Script.f_Find("apk add --no-cache ca-certificates") >= 0);
		}

		{
			DMibTestPath("macOS Brew");

			CStr Error;
			CStr Script = fg_AppManager_BuildOSDependencyInstallScript({.m_ID = "macos"}, Dependencies, Error);
			DMibExpectTrue(Error.f_IsEmpty());
			DMibExpectTrue(Script.f_Find("install openssl@3") >= 0);
			DMibExpectTrue(Script.f_Find("ca-certificates") < 0);
		}

		{
			DMibTestPath("No Match");

			CStr Error;
			TCMap<CStr, TCVector<CStr>> MacDependencies;
			MacDependencies["macOS"] = fg_CreateVector<CStr>("openssl@3");

			CStr Script = fg_AppManager_BuildOSDependencyInstallScript({.m_ID = "ubuntu", .m_Like = {"debian"}}, MacDependencies, Error);
			DMibExpectTrue(Error.f_IsEmpty());
			DMibExpectTrue(Script.f_IsEmpty());
		}

		{
			DMibTestPath("Unknown Manager");

			CStr Error;
			CStr Script = fg_AppManager_BuildOSDependencyInstallScript({.m_ID = "haiku"}, Dependencies, Error);
			DMibExpectFalse(Error.f_IsEmpty());
			DMibExpectTrue(Script.f_IsEmpty());
		}

		{
			DMibTestPath("Invalid Package Name");

			CStr Error;
			TCMap<CStr, TCVector<CStr>> BadDependencies;
			BadDependencies["Linux"] = fg_CreateVector<CStr>("curl; rm -rf /");

			CStr Script = fg_AppManager_BuildOSDependencyInstallScript({.m_ID = "ubuntu", .m_Like = {"debian"}}, BadDependencies, Error);
			DMibExpectFalse(Error.f_IsEmpty());
			DMibExpectTrue(Script.f_IsEmpty());
		}

		{
			DMibTestPath("Apt Removal Suffix");

			CStr Error;
			TCMap<CStr, TCVector<CStr>> RemovalDependencies;
			RemovalDependencies["Linux"] = fg_CreateVector<CStr>("openssh-server-");

			CStr Script = fg_AppManager_BuildOSDependencyInstallScript({.m_ID = "ubuntu", .m_Like = {"debian"}}, RemovalDependencies, Error);
			DMibExpectFalse(Error.f_IsEmpty());
			DMibExpectTrue(Script.f_IsEmpty());
		}

		co_return {};
	};

#ifndef DPlatformFamily_Windows
	DMibTestSuite("Apt Install Script")
	{
		CStr RootDirectory = CFile::fs_GetProgramDirectory() / "AppManagerAptInstallScript";
		CStr StubDirectory = RootDirectory / "Stubs";

		fg_TestAddCleanupPath(RootDirectory);

		if (CFile::fs_FileExists(RootDirectory))
			CFile::fs_DeleteDirectoryRecursive(RootDirectory);

		CFile::fs_CreateDirectory(StubDirectory);

		// The stubs stand in for the package tools: they print the fixtures and log their invocations
		auto fWriteStub = [&](CStr const &_Name, CStr const &_Body)
			{
				CStr Path = StubDirectory / _Name;
				CFile::fs_WriteStringToFile(Path, "#!/bin/sh\necho \"{} $*\" >> '{}'\n{}"_f << _Name << (RootDirectory / "Log") << _Body, false);
				CFile::fs_SetAttributes(Path, EFileAttrib_UnixAttributesValid | EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute);
			}
		;

		fWriteStub("dpkg-query", "cat '{}'\n"_f << (RootDirectory / "Status"));
		fWriteStub("apt-mark", "if [ \"$1\" = showauto ]; then\n\tcat '{}'\nfi\n"_f << (RootDirectory / "Automatic"));
		fWriteStub("apt-get", "");
		fWriteStub("apt-cache", "grep -Fx -- \"$3\" '{}' || true\n"_f << (RootDirectory / "Known"));
		fWriteStub("dpkg", "if [ \"$1\" = --print-architecture ]; then\n\techo amd64\nelse\n\techo arm64\nfi\n");

		CFile::fs_WriteStringToFile(RootDirectory / "Known", "missing\nother\n", false);

		auto fRun = [&](TCVector<CStr> const &_Packages, CStr const &_Status, CStr const &_Automatic, uint32 &o_ExitCode) -> CStr
			{
				CFile::fs_WriteStringToFile(RootDirectory / "Status", _Status, false);
				CFile::fs_WriteStringToFile(RootDirectory / "Automatic", _Automatic, false);
				CFile::fs_WriteStringToFile(RootDirectory / "Log", "", false);

				TCMap<CStr, TCVector<CStr>> Dependencies;
				Dependencies["Linux"] = _Packages;

				CStr Error;
				CStr Script = fg_AppManager_BuildOSDependencyInstallScript({.m_ID = "ubuntu", .m_Like = {"debian"}}, Dependencies, Error);
				DMibAssertTrue(Error.f_IsEmpty());

				CStr StdOut;
				CStr StdErr;
				o_ExitCode = 1;
				DMibAssertTrue(CProcessLaunch::fs_LaunchBlock("/bin/sh", fg_CreateVector<CStr>("-ec", "PATH='{}':\"$PATH\"\n{}"_f << StubDirectory << Script), StdOut, StdErr, o_ExitCode));

				return CFile::fs_ReadStringFromFile(RootDirectory / "Log");
			}
		;

		{
			DMibTestPath("Installed");

			// A held package is installed, as is a name with its architecture
			uint32 ExitCode = 1;
			CStr Log = fRun
				(
					fg_CreateVector<CStr>("held", "libssl3:amd64", "automatic")
					, "hi amd64||held|\nii amd64|same|libssl3|\nii amd64||automatic|\nrc amd64||removed|\n"
					, "automatic\n"
					, ExitCode
				)
			;
			DMibExpect(ExitCode, ==, 0u);
			DMibExpectTrue(Log.f_Find("apt-mark showauto automatic held libssl3\n") >= 0);
			DMibExpectTrue(Log.f_Find("apt-mark manual automatic\n") >= 0);
			DMibExpect(Log.f_Find("apt-get"), <, 0);
		}

		{
			DMibTestPath("Foreign Architecture");

			// A library that is installed only for another architecture is installed for the native one, while a package that isn't
			// Multi-Arch: same satisfies its name in any architecture
			uint32 ExitCode = 1;
			CStr Log = fRun(fg_CreateVector<CStr>("missing", "other"), "ii arm64|same|missing|\nii arm64|foreign|other|\n", "", ExitCode);
			DMibExpect(ExitCode, ==, 0u);
			DMibExpectTrue(Log.f_Find("apt-mark showauto other:arm64\n") >= 0);
			DMibExpectTrue(Log.f_Find(" install -y -qq --no-install-recommends --no-remove missing\n") >= 0);
		}

		{
			DMibTestPath("Virtual Provider");

			// The package that provides a virtual name is marked as installed manually
			uint32 ExitCode = 1;
			CStr Log = fRun(fg_CreateVector<CStr>("virtual"), "ii amd64||provider|virtual (= 1.0), other\n", "provider\n", ExitCode);
			DMibExpect(ExitCode, ==, 0u);
			DMibExpectTrue(Log.f_Find("apt-mark showauto provider\n") >= 0);
			DMibExpectTrue(Log.f_Find("apt-mark manual provider\n") >= 0);
			DMibExpect(Log.f_Find("apt-get"), <, 0);
		}

		{
			DMibTestPath("Missing");

			// The installed packages are marked as installed manually also when others are missing, and the missing names are checked
			// without their architectures, which the install keeps
			uint32 ExitCode = 1;
			CStr Log = fRun(fg_CreateVector<CStr>("automatic", "missing:arm64", "other"), "ii amd64||automatic|\n", "automatic\n", ExitCode);
			DMibExpect(ExitCode, ==, 0u);
			DMibExpectTrue(Log.f_Find("apt-mark manual automatic\n") >= 0);
			DMibExpectTrue(Log.f_Find(" update -qq\n") >= 0);
			DMibExpectTrue(Log.f_Find(" install -y -qq --no-install-recommends --no-remove missing:arm64 other\n") >= 0);
		}

		{
			DMibTestPath("Unknown Architecture");

			uint32 ExitCode = 0;
			CStr Log = fRun(fg_CreateVector<CStr>("missing:sparc"), "", "", ExitCode);
			DMibExpect(ExitCode, !=, 0u);
			DMibExpect(Log.f_Find(" install "), <, 0);
		}

		{
			DMibTestPath("Unknown Name");

			uint32 ExitCode = 0;
			CStr Log = fRun(fg_CreateVector<CStr>("unknown"), "", "", ExitCode);
			DMibExpect(ExitCode, !=, 0u);
			DMibExpect(Log.f_Find(" install "), <, 0);
		}
	};
#endif

	DMibTestSuite("Low Port Executables To Grant")
	{
		CStr RootDirectory = CFile::fs_GetProgramDirectory() / "AppManagerLowPortExecutablesToGrant";
		CStr ApplicationDirectory = RootDirectory / "App";
		CStr ExecutablePath = ApplicationDirectory / "bin/Server";

		fg_TestAddCleanupPath(RootDirectory);

		if (CFile::fs_FileExists(RootDirectory))
			CFile::fs_DeleteDirectoryRecursive(RootDirectory);

		CFile::fs_CreateDirectory(ApplicationDirectory / "bin");
		CFile::fs_WriteStringToFile(ExecutablePath, "Installed", false);
		CFile::fs_WriteStringToFile(RootDirectory / "Other", "Other", false);

		CStr InstalledDigest = fg_AppManager_LowPortExecutableDigest(ExecutablePath);

		{
			DMibTestPath("Installed");
			DMibExpect(fg_AppManager_InstalledLowPortExecutablePath(ApplicationDirectory, "bin/Server", InstalledDigest), ==, ExecutablePath);
		}

		{
			DMibTestPath("Missing");
			DMibExpect(fg_AppManager_InstalledLowPortExecutablePath(ApplicationDirectory, "bin/Missing", InstalledDigest), ==, "");
		}

		{
			DMibTestPath("Changed");
			CFile::fs_WriteStringToFile(ExecutablePath, "Changed", false);
			DMibExpect(fg_AppManager_InstalledLowPortExecutablePath(ApplicationDirectory, "bin/Server", InstalledDigest), ==, "");
		}

		{
			DMibTestPath("Replaced");
			CFile::fs_DeleteFile(ExecutablePath);
			CFile::fs_RenameFile(RootDirectory / "Other", ExecutablePath);
			DMibExpect(fg_AppManager_InstalledLowPortExecutablePath(ApplicationDirectory, "bin/Server", InstalledDigest), ==, "");
		}

		{
			DMibTestPath("Restored");
			CFile::fs_WriteStringToFile(ExecutablePath, "Installed", false);
			DMibExpect(fg_AppManager_InstalledLowPortExecutablePath(ApplicationDirectory, "bin/Server", InstalledDigest), ==, ExecutablePath);
		}

#ifndef DPlatformFamily_Windows
		{
			DMibTestPath("FIFO");

			// Reading a FIFO without a writer would block
			CProcessLaunch::fs_LaunchTool("mkfifo", fg_CreateVector<CStr>(ApplicationDirectory / "bin/Pipe"));
			DMibExpect(fg_AppManager_InstalledLowPortExecutablePath(ApplicationDirectory, "bin/Pipe", InstalledDigest), ==, "");
		}
#endif
	};
}

DMibTestRegister(CAppManager_OSDependencies_Tests, Malterlib::Cloud);
