// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Test_Malterlib_Cloud_AppManager.h"

struct CAppManager_OSDependencies_Tests : public NMib::NTest::CTest
{
	void f_DoTests()
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
				DMibExpectTrue(Script.f_Find("apt-get update") >= 0);
				DMibExpectTrue(Script.f_Find("apt-get install -y -qq --no-install-recommends ca-certificates curl libxml2") >= 0);
			}

			{
				DMibTestPath("Debian Via Like");

				CStr Error;
				TCMap<CStr, TCVector<CStr>> LikeDependencies;
				LikeDependencies["debian"] = fg_CreateVector<CStr>("curl");

				CStr Script = fg_AppManager_BuildOSDependencyInstallScript({.m_ID = "ubuntu", .m_Like = {"debian"}}, LikeDependencies, Error);
				DMibExpectTrue(Error.f_IsEmpty());
				DMibExpectTrue(Script.f_Find("apt-get install -y -qq --no-install-recommends curl") >= 0);
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
				DMibExpectTrue(Script.f_Find("apk add ca-certificates") >= 0);
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

			co_return {};
		};
	}
};

DMibTestRegister(CAppManager_OSDependencies_Tests, Malterlib::Cloud);
