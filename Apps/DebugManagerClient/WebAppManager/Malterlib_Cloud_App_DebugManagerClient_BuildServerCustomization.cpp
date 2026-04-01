// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

struct CWebAppManagerCustomization_DebugManagerClient: public ICWebAppManagerCustomization
{
	void f_SetupPrerequisites(TCSet<CStr> const &_Tags, TCMap<CStr, CUser> const &_Users) override
	{
	}

	void f_CalculateSettings
		(
			TCMap<CStr, CStr> &o_Settings
			, CJsonSorted &o_MeteorSettings
			, CStr const &_PackageName
			, CWebAppManagerOptions::CPackage const &_PackageOptions
			, ICWebAppManager const &_WebAppManager
		) override
	{
	}

	void f_ManipulateNginxConfig
		(
			CStr &o_Config
			, CStr const &_FastCGIFile
			, TCMap<CStr, CStr> const &_PackageIPs
			, ICWebAppManager const &_WebAppManager
		)
		override
	{
	}

private:

};

TCSharedPointer<ICWebAppManagerCustomization> NMib::NWebApp::NWebAppManager::fg_CreateWebAppManagerCustomization()
{
	return fg_Construct<CWebAppManagerCustomization_DebugManagerClient>();
}
