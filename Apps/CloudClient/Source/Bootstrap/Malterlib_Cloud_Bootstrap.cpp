// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Encoding/JsonShortcuts>

#include "Malterlib_Cloud_Bootstrap.h"
#include "Malterlib_Cloud_Bootstrap_Aws.h"
#include "Malterlib_Cloud_Bootstrap_Prompts.h"
#include "Malterlib_Cloud_Bootstrap_ConfigUI.h"
#include "Malterlib_Cloud_Bootstrap_Packages.h"

namespace NMib::NCloud::NBootstrap
{
	TCFuture<uint32> fg_Bootstrap_MalterlibCloud
		(
			CEJsonSorted _Params
			, TCSharedPointer<CCommandLineControl> _pCommandLine
			, CStr _RootDirectory
			, TCActor<NConcurrency::CDistributedActorTrustManager> _TrustManager
		)
	{
		auto CheckDestroy = co_await fg_CurrentActorCheckDestroyedOnResume();

		using namespace NStr;

		if (_pCommandLine->m_CommandLineWidth < 130)
		{
			*_pCommandLine %= "Error: Terminal width must be at least 130 columns. Current width: {} columns.\n"_f << _pCommandLine->m_CommandLineWidth;
			co_return 1;
		}

		if (_pCommandLine->m_CommandLineHeight < 48)
		{
			*_pCommandLine %= "Error: Terminal height must be at least 48 lines. Current height: {} lines.\n"_f << _pCommandLine->m_CommandLineHeight;
			co_return 1;
		}

		CStr Provider = _Params["Provider"].f_String();
		CStr Region = _Params["Region"].f_String();
		CStr Environment = _Params["Environment"].f_String();
		CStr AwsAccessKeyId = _Params["AwsAccessKeyId"].f_String();
		CStr AwsSecretAccessKey = _Params["AwsSecretAccessKey"].f_String();

		Provider = co_await fg_PromptWithDefault(_pCommandLine, Provider, "Cloud provider", "Aws");

		if (Provider != "Aws")
		{
			*_pCommandLine %= "Error: Unsupported provider '{}'. Currently only 'Aws' is supported.\n"_f << Provider;
			co_return 1;
		}

		// Get AWS credentials
		NWeb::CAwsCredentials Credentials = co_await fg_PromptForAwsCredentials(_pCommandLine, AwsAccessKeyId, AwsSecretAccessKey, "us-east-1");

		if (Credentials.m_AccessKeyID.f_IsEmpty())
			co_return 1;

		// Use default region if not specified (can be changed in config UI)
		if (Region.f_IsEmpty())
			Region = "us-east-1";

		// Configure deployment with interactive UI
		CBootstrapConfig Config;
		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Configuration cancelled");

			Config = co_await fg_ConfigureBootstrapDeployment(_pCommandLine, Credentials, Region, _RootDirectory);
		}

		if (Config.m_Region.f_IsEmpty())
		{
			*_pCommandLine += "Configuration cancelled.\n";
			co_return 1;
		}

		Environment = co_await fg_PromptWithDefault(_pCommandLine, Environment, "Environment name", "Production");

		// Display final configuration summary
		*_pCommandLine += "\n{}Bootstrap Configuration Summary{}\n"_f
			<< _pCommandLine->f_AnsiEncoding().f_Bold()
			<< _pCommandLine->f_AnsiEncoding().f_Default()
		;
		*_pCommandLine += "  Provider: {}\n"_f << Provider;
		*_pCommandLine += "  Region: {}\n"_f << Config.m_Region;
		*_pCommandLine += "  Environment: {}\n"_f << Environment;
		*_pCommandLine += "  Isolation: {}\n"_f << CBootstrapConfig::fs_GetIsolationLevelDisplayName(Config.m_IsolationLevel);
		*_pCommandLine += "  Storage: {}\n"_f << CBootstrapConfig::fs_GetStorageIsolationDisplayName(Config.m_StorageIsolation);
		*_pCommandLine += "  Encryption: {}\n"_f << (Config.m_bStorageEncryption ? "Enabled" : "Disabled");
		*_pCommandLine += "  Snapshots: {}\n"_f << CBootstrapConfig::fs_GetSnapshotLevelDisplayName(Config.m_SnapshotLevel);
		*_pCommandLine += "  KeyManagers: {}\n"_f << Config.m_KeyManagerCount;
		*_pCommandLine += "  Estimated Monthly Cost: ${fe2}\n"_f << Config.f_CalculateTotalMonthlyCost();

		// Show custom configuration if any
		if (Config.f_HasAnyCustomizations())
		{
			*_pCommandLine += "\n{}Custom Configuration:{}\n"_f
				<< _pCommandLine->f_AnsiEncoding().f_Bold()
				<< _pCommandLine->f_AnsiEncoding().f_Default()
			;

			for (auto const &App : Config.m_Applications)
			{
				if (!App.f_HasCustomization())
					continue;

				CStr Line = "  {}: "_f << App.m_Host;
				if (App.m_Name != "AppManager")
					Line += "{} - "_f << App.m_Name;

				TCVector<CStr> Customizations;

				if (App.m_InstanceTypeOverride)
				{
					CStr ArchStr = "-";
					CStr MemoryStr = "-";
					uint32 VCPUs = 0;
					if (auto *pInfo = Config.m_InstanceInfo.f_FindEqual(*App.m_InstanceTypeOverride))
					{
						ArchStr = CBootstrapConfig::fs_GetArchitectureDisplayName(pInfo->m_Architecture);
						MemoryStr = "{} GB"_f << pInfo->m_MemoryGB;
						VCPUs = pInfo->m_VCPUs;
					}
					Customizations.f_Insert("Instance: {} ({} vCPU, {}, {})"_f
						<< *App.m_InstanceTypeOverride
						<< VCPUs
						<< MemoryStr
						<< ArchStr)
					;
				}

				if (App.m_StorageGBOverride)
					Customizations.f_Insert("Storage: {} GB"_f << *App.m_StorageGBOverride);

				if (App.m_BandwidthMBpsOverride)
					Customizations.f_Insert("Bandwidth: {} MB/s"_f << *App.m_BandwidthMBpsOverride);

				if (App.m_IOPsOverride)
					Customizations.f_Insert("IOPs: {}"_f << *App.m_IOPsOverride);

				if (App.m_SnapshotOverride)
					Customizations.f_Insert("Snapshots: {}"_f << (*App.m_SnapshotOverride ? "Yes" : "No"));

				for (mint i = 0; i < Customizations.f_GetLen(); ++i)
				{
					if (i > 0)
						Line += ", ";
					Line += Customizations[i];
				}

				*_pCommandLine += Line + "\n";
			}
		}

		// Configure packages
		auto PackagesResult = co_await fg_ConfigurePackages
			(
				_pCommandLine
				, _TrustManager
				, _RootDirectory
				, Config
			)
		;

		if (!PackagesResult)
		{
			*_pCommandLine += "Package configuration cancelled.\n";
			co_return 1;
		}

		Config = *PackagesResult;

		// Show package configuration summary
		*_pCommandLine += "\n{}Package Configuration{}\n"_f
			<< _pCommandLine->f_AnsiEncoding().f_Bold()
			<< _pCommandLine->f_AnsiEncoding().f_Default()
		;

		if (Config.m_PackageConfig.m_Source == EPackageSource::mc_LocalDirectory)
		{
			*_pCommandLine += "  Source: Local Directory\n";
			*_pCommandLine += "  Path: {}\n"_f << Config.m_PackageConfig.m_LocalPackageDirectory;
		}
		else
		{
			*_pCommandLine += "  Source: VersionManager\n";
			*_pCommandLine += "  Branch: {}\n"_f << (Config.m_PackageConfig.m_Branch.f_IsEmpty() ? "Any" : Config.m_PackageConfig.m_Branch);
			*_pCommandLine += "  Tag: {}\n"_f << (Config.m_PackageConfig.m_Tag.f_IsEmpty() ? "Any" : Config.m_PackageConfig.m_Tag);

			mint nApps = 0;
			mint nPlatforms = 0;
			for (auto const &bSelected : Config.m_PackageConfig.m_SelectedApplications)
			{
				if (bSelected)
					++nApps;
			}
			nPlatforms = Config.m_PackageConfig.m_SelectedPlatforms.f_GetLen();

			*_pCommandLine += "  Applications: {} selected\n"_f << nApps;
			*_pCommandLine += "  Platforms: {} selected\n"_f << nPlatforms;
		}

		*_pCommandLine += "\nBootstrap provisioning not yet implemented.\n";

		co_return 0;
	}
}
