// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_Bootstrap_Config.h"
#include "Malterlib_Cloud_Bootstrap_ConfigUI.h"

namespace NMib::NCloud::NBootstrap
{
	using namespace NStr;

	// Memory requirements per application (in GB)
	static constexpr pfp64 gc_MemoryGB_KeyManager = 0.5;
	static constexpr pfp64 gc_MemoryGB_CloudManager = 4.0;
	static constexpr pfp64 gc_MemoryGB_VersionManager = 0.5;
	static constexpr pfp64 gc_MemoryGB_SecretsManager = 0.5;
	static constexpr pfp64 gc_MemoryGB_OS = 0.5;	// Base OS overhead on host

	// Fallback instance type when no pricing data available
	static constexpr ch8 const *gc_pFallbackInstanceType = "t3.medium";

	// Get memory requirement for an application type
	static pfp64 fg_GetMemoryRequirement(EApplicationType _Type)
	{
		switch (_Type)
		{
		case EApplicationType::mc_KeyManager:
			return gc_MemoryGB_KeyManager;
		case EApplicationType::mc_CloudManager:
			return gc_MemoryGB_CloudManager;
		case EApplicationType::mc_VersionManager:
			return gc_MemoryGB_VersionManager;
		case EApplicationType::mc_SecretsManager:
			return gc_MemoryGB_SecretsManager;
		case EApplicationType::mc_AppManager:
			return gc_MemoryGB_OS;
		}
		return 0;
	}

	// Default storage sizes (GB)
	static constexpr uint32 gc_DefaultStorageGB_RootVolume = 20;		// Base OS storage for AppManager
	static constexpr uint32 gc_DefaultStorageGB_KeyManager = 5;
	static constexpr uint32 gc_DefaultStorageGB_CloudManager = 130;
	static constexpr uint32 gc_DefaultStorageGB_VersionManager = 250;
	static constexpr uint32 gc_DefaultStorageGB_SecretsManager = 5;

	// Hours per month for cost calculations
	static constexpr pfp64 const gc_HoursPerMonth = 730.0;

	fp64 CBootstrapConfig::f_CalculateTotalMonthlyCost() const
	{
		fp64 Total = 0;

		// Sum application costs
		for (auto const &App : m_Applications)
		{
			// Instance cost (only for apps that own root volume - one instance per host)
			if (App.m_bOwnsRootVolume)
				Total += App.m_InstanceCostPerHour * gc_HoursPerMonth;

			// Storage cost
			Total += App.m_StorageCostPerMonth;

			// Snapshot cost
			Total += App.m_SnapshotCostPerMonth;
		}

		// Sum other costs
		for (auto const &Other : m_OtherCosts)
			Total += Other.m_MonthlyCost;

		return Total;
	}

	void CBootstrapConfig::f_RecalculateLayout()
	{
		m_Applications.f_Clear();
		m_OtherCosts.f_Clear();

		// App configuration within a host
		struct CAppConfig
		{
			CStr m_Name;
			EApplicationType m_Type;
			uint32 m_StorageGB;
			bool m_bCritical;
		};

		// Build host configurations based on isolation level
		struct CHostConfig
		{
			CStr m_Name;
			CStr m_InstanceType;	// Calculated based on total memory requirements
			TCVector<CAppConfig> m_Apps;
		};
		TCVector<CHostConfig> HostConfigs;

		auto fGetOrAddHost = [&](CStr _Name) -> CHostConfig &
			{
				for (auto &Host : HostConfigs)
				{
					if (Host.m_Name == _Name)
						return Host;
				}
				CHostConfig &Host = HostConfigs.f_Insert();
				Host.m_Name = _Name;
				return Host;
			}
		;

		auto fAddAppToHost = [&](CStr _HostName, CStr _AppName, EApplicationType _Type, uint32 _StorageGB, bool _bCritical)
			{
				CHostConfig &Host = fGetOrAddHost(_HostName);
				CAppConfig &App = Host.m_Apps.f_Insert();
				App.m_Name = _AppName;
				App.m_Type = _Type;
				App.m_StorageGB = _StorageGB;
				App.m_bCritical = _bCritical;
			}
		;

		// Configure hosts based on isolation level
		switch (m_IsolationLevel)
		{
		case EIsolationLevel::mc_Maximum:
			{
				// Option 1: One app per host
				fAddAppToHost("KeyManager A", "KeyManager", EApplicationType::mc_KeyManager, gc_DefaultStorageGB_KeyManager, true);

				if (m_KeyManagerCount >= 2)
					fAddAppToHost("KeyManager B", "KeyManager", EApplicationType::mc_KeyManager, gc_DefaultStorageGB_KeyManager, true);

				if (m_KeyManagerCount >= 3)
					fAddAppToHost("KeyManager C", "KeyManager", EApplicationType::mc_KeyManager, gc_DefaultStorageGB_KeyManager, true);

				fAddAppToHost("CloudManager", "CloudManager", EApplicationType::mc_CloudManager, gc_DefaultStorageGB_CloudManager, false);
				fAddAppToHost("VersionManager", "VersionManager", EApplicationType::mc_VersionManager, gc_DefaultStorageGB_VersionManager, false);
				fAddAppToHost("SecretsManager", "SecretsManager", EApplicationType::mc_SecretsManager, gc_DefaultStorageGB_SecretsManager, true);
				break;
			}

		case EIsolationLevel::mc_Moderate:
			{
				// Option 2: KeyManagers isolated, management apps consolidated
				fAddAppToHost("KeyManager A", "KeyManager", EApplicationType::mc_KeyManager, gc_DefaultStorageGB_KeyManager, true);

				if (m_KeyManagerCount >= 2)
					fAddAppToHost("KeyManager B", "KeyManager", EApplicationType::mc_KeyManager, gc_DefaultStorageGB_KeyManager, true);

				if (m_KeyManagerCount >= 3)
					fAddAppToHost("KeyManager C", "KeyManager", EApplicationType::mc_KeyManager, gc_DefaultStorageGB_KeyManager, true);

				fAddAppToHost("Management", "CloudManager", EApplicationType::mc_CloudManager, gc_DefaultStorageGB_CloudManager, false);
				fAddAppToHost("Management", "VersionManager", EApplicationType::mc_VersionManager, gc_DefaultStorageGB_VersionManager, false);
				fAddAppToHost("Management", "SecretsManager", EApplicationType::mc_SecretsManager, gc_DefaultStorageGB_SecretsManager, true);
				break;
			}

		case EIsolationLevel::mc_Minimal:
			{
				// Option 3: All core apps on one host
				fAddAppToHost("Management", "KeyManager", EApplicationType::mc_KeyManager, gc_DefaultStorageGB_KeyManager, true);
				fAddAppToHost("Management", "CloudManager", EApplicationType::mc_CloudManager, gc_DefaultStorageGB_CloudManager, false);
				fAddAppToHost("Management", "VersionManager", EApplicationType::mc_VersionManager, gc_DefaultStorageGB_VersionManager, false);
				fAddAppToHost("Management", "SecretsManager", EApplicationType::mc_SecretsManager, gc_DefaultStorageGB_SecretsManager, true);

				if (m_KeyManagerCount >= 2)
					fAddAppToHost("KeyManager B", "KeyManager", EApplicationType::mc_KeyManager, gc_DefaultStorageGB_KeyManager, true);

				if (m_KeyManagerCount >= 3)
					fAddAppToHost("KeyManager C", "KeyManager", EApplicationType::mc_KeyManager, gc_DefaultStorageGB_KeyManager, true);
				break;
			}
		}

		// Calculate instance type for each host based on total memory requirements
		for (auto &Host : HostConfigs)
		{
			// Sum memory requirements: OS + all apps on this host
			fp64 TotalMemoryGB = gc_MemoryGB_OS;
			for (auto const &AppConfig : Host.m_Apps)
				TotalMemoryGB += fg_GetMemoryRequirement(AppConfig.m_Type);

			// Find cheapest instance meeting the requirement
			Host.m_InstanceType = f_FindCheapestInstance(TotalMemoryGB);

			// Fallback if no instance found (e.g., no pricing data yet)
			if (Host.m_InstanceType.f_IsEmpty())
				Host.m_InstanceType = gc_pFallbackInstanceType;
		}

		// Generate application entries from host configs
		for (auto const &Host : HostConfigs)
		{
			// Add AppManager for this host (owns root volume)
			{
				CDeploymentApplication &App = m_Applications.f_Insert();
				App.m_Name = "AppManager";
				App.m_Type = EApplicationType::mc_AppManager;
				App.m_Host = Host.m_Name;
				App.m_InstanceType = Host.m_InstanceType;
				App.m_bOwnsRootVolume = true;
				App.m_bIsCritical = false;

				// Root volume storage depends on isolation mode
				// KeyManagers use no extra storage (they fit in base OS allocation)
				App.m_StorageGB = gc_DefaultStorageGB_RootVolume;

				if (m_StorageIsolation == EStorageIsolation::mc_Shared)
				{
					// Shared: root volume contains non-KeyManager app storage
					for (auto const &AppConfig : Host.m_Apps)
					{
						if (AppConfig.m_Type != EApplicationType::mc_KeyManager && AppConfig.m_Type != EApplicationType::mc_SecretsManager)
						{
							App.m_StorageGB += AppConfig.m_StorageGB;
						}
					}
				}
				else if (m_StorageIsolation == EStorageIsolation::mc_Separate)
				{
					// Separate: KeyManagers stay on root but don't need extra storage
					// Non-KeyManager apps get their own volumes
				}
				// ESeparateAll: all apps get separate volumes, root is just base OS

				// Snapshots for AppManager
				switch (m_SnapshotLevel)
				{
				case ESnapshotLevel::mc_None:
					App.m_bHasSnapshot = false;
					break;
				case ESnapshotLevel::mc_Critical:
					switch (m_StorageIsolation)
					{
					case EStorageIsolation::mc_Shared:
						App.m_bHasSnapshot = false;
						for (auto const &AppConfig : Host.m_Apps)
						{
							if (AppConfig.m_bCritical)
							{
								App.m_bHasSnapshot = true;
								break;
							}
						}
						break;

					case EStorageIsolation::mc_Separate:
						App.m_bHasSnapshot = false;
						for (auto const &AppConfig : Host.m_Apps)
						{
							if (AppConfig.m_Type == EApplicationType::mc_KeyManager)
							{
								App.m_bHasSnapshot = true;
								break;
							}
						}
						break;

					case EStorageIsolation::mc_SeparateAll:
						App.m_bHasSnapshot = false;
						break;
					}
					break;
				case ESnapshotLevel::mc_All:
					App.m_bHasSnapshot = true;
					break;
				}
			}

			// Add each application on this host
			for (auto const &AppConfig : Host.m_Apps)
			{
				CDeploymentApplication &App = m_Applications.f_Insert();
				App.m_Name = AppConfig.m_Name;
				App.m_Type = AppConfig.m_Type;
				App.m_Host = Host.m_Name;
				App.m_InstanceType = Host.m_InstanceType;
				App.m_bOwnsRootVolume = false;
				App.m_bIsCritical = AppConfig.m_bCritical;

				// Storage depends on isolation mode
				bool bIsKeyManager = (AppConfig.m_Type == EApplicationType::mc_KeyManager);

				switch (m_StorageIsolation)
				{
				case EStorageIsolation::mc_Shared:
					// All apps share root volume
					App.m_StorageGB = 0;
					break;

				case EStorageIsolation::mc_Separate:
					// KeyManagers stay on root, others get separate volumes
					App.m_StorageGB = bIsKeyManager ? 0 : AppConfig.m_StorageGB;
					break;

				case EStorageIsolation::mc_SeparateAll:
					// All apps including KeyManagers get separate volumes
					App.m_StorageGB = AppConfig.m_StorageGB;
					break;
				}

				// Snapshots
				switch (m_SnapshotLevel)
				{
				case ESnapshotLevel::mc_None:
					App.m_bHasSnapshot = false;
					break;
				case ESnapshotLevel::mc_Critical:
					App.m_bHasSnapshot = App.m_bIsCritical && App.m_StorageGB > 0;
					break;
				case ESnapshotLevel::mc_All:
					App.m_bHasSnapshot = App.m_StorageGB > 0;
					break;
				}
			}
		}

		// Add other costs based on NAT configuration
		// Count hosts that need public IPs (CloudManager or VersionManager hosts, or all hosts if no NAT)
		mint nPublicServiceHosts = 0;
		for (auto const &Host : HostConfigs)
		{
			for (auto const &App : Host.m_Apps)
			{
				if (App.m_Type == EApplicationType::mc_CloudManager || App.m_Type == EApplicationType::mc_VersionManager)
				{
					++nPublicServiceHosts;
					break;	// Only count each host once
				}
			}
		}

		mint nPublicIPs = 0;
		fp64 NATMonthlyCost = 0;
		CStr NATDescription;

		switch (m_NATConfiguration)
		{
		case ENATConfiguration::mc_None:
			// No NAT - all hosts need public IPs
			nPublicIPs = (mint)HostConfigs.f_GetLen();
			break;

		case ENATConfiguration::mc_Single:
			{
				// Single NAT Gateway in one AZ + its public IP + public service hosts
				nPublicIPs = 1 + nPublicServiceHosts;	// NAT Gateway IP + service hosts
				fp64 HourlyCost = m_NATGatewayPricePerHour * gc_HoursPerMonth;
				fp64 DataCost = m_NATGatewayDataProcessedPerGB * (fp64)m_NATDataTransferGBPerMonth;
				NATMonthlyCost = HourlyCost + DataCost;
				NATDescription = "NAT Gateway ({} GB)"_f << m_NATDataTransferGBPerMonth;
				break;
			}

		case ENATConfiguration::mc_Regional:
			{
				// Regional NAT Gateway spanning all AZs, uses single public IP
				nPublicIPs = 1 + nPublicServiceHosts;	// Single NAT Gateway IP + service hosts
				// Regional NAT is charged per active AZ
				fp64 RegionalHourlyRate = (m_RegionalNATGatewayPricePerHour > 0)
					? m_RegionalNATGatewayPricePerHour
					: m_NATGatewayPricePerHour
				;
				fp64 RegionalDataRate = (m_RegionalNATGatewayDataProcessedPerGB > 0)
					? m_RegionalNATGatewayDataProcessedPerGB
					: m_NATGatewayDataProcessedPerGB
				;
				fp64 HourlyCost = RegionalHourlyRate * gc_HoursPerMonth * (fp64)f_GetAvailabilityZoneCount();
				fp64 DataCost = RegionalDataRate * (fp64)m_NATDataTransferGBPerMonth;
				NATMonthlyCost = HourlyCost + DataCost;
				NATDescription = "Regional NAT ({} AZs, {} GB)"_f << f_GetAvailabilityZoneCount() << m_NATDataTransferGBPerMonth;
				break;
			}

		case ENATConfiguration::mc_PerAZ:
			{
				// One NAT Gateway per AZ, each with its own public IP
				nPublicIPs = (mint)f_GetAvailabilityZoneCount() + nPublicServiceHosts;
				fp64 HourlyCost = m_NATGatewayPricePerHour * gc_HoursPerMonth * (fp64)f_GetAvailabilityZoneCount();
				fp64 DataCost = m_NATGatewayDataProcessedPerGB * (fp64)m_NATDataTransferGBPerMonth;
				NATMonthlyCost = HourlyCost + DataCost;
				NATDescription = "NAT Gateway ({} AZs, {} GB)"_f << f_GetAvailabilityZoneCount() << m_NATDataTransferGBPerMonth;
				break;
			}
		}

		// NAT Gateway cost
		if (m_NATConfiguration != ENATConfiguration::mc_None)
		{
			COtherCost &Cost = m_OtherCosts.f_Insert();
			Cost.m_Description = NATDescription;
			Cost.m_MonthlyCost = NATMonthlyCost;
		}

		// Public IPv4 addresses
		{
			COtherCost &Cost = m_OtherCosts.f_Insert();
			Cost.m_Description = "Public IPv4 ({} addresses)"_f << nPublicIPs;
			Cost.m_MonthlyCost = m_PublicIPPricePerHour * gc_HoursPerMonth * (fp64)nPublicIPs;
		}

		// S3 storage for version artifacts (estimated 50 GB)
		{
			COtherCost &Cost = m_OtherCosts.f_Insert();
			Cost.m_Description = "S3 Storage (50 GB)";
			Cost.m_MonthlyCost = 2.0;	// Approximately $0.023/GB * 50 GB + requests
		}

		// Update costs based on pricing
		f_UpdateCosts();
	}

	void CBootstrapConfig::f_UpdateCosts()
	{
		for (auto &App : m_Applications)
		{
			// Get effective instance type (uses override if set)
			CStr const &EffectiveInstanceType = App.f_GetInstanceType();

			// Look up instance price based on pricing selection
			App.m_InstanceCostPerHour = f_GetEffectiveInstancePrice(EffectiveInstanceType);

			// Look up instance metadata (vCPUs, memory)
			if (auto *pInfo = m_InstanceInfo.f_FindEqual(EffectiveInstanceType))
			{
				App.m_VCPUs = pInfo->m_VCPUs;
				App.m_MemoryGB = pInfo->m_MemoryGB;
			}
			else
			{
				App.m_VCPUs = 0;
				App.m_MemoryGB = 0;
			}

			// Calculate storage cost using effective values (getters apply overrides)
			uint32 nStorageGB = App.f_GetStorageGB();
			uint32 nBandwidthMBps = App.f_GetBandwidthMBps();
			uint32 nIOPs = App.f_GetIOPs();

			if (nStorageGB > 0)
			{
				// Base GP3 storage cost
				fp64 BaseCost = m_EBSPricePerGBMonth * (fp64)nStorageGB;

				// Additional IOPS cost (above 3000 baseline)
				fp64 AdditionalIOPS = (nIOPs > 3000) ? (fp64)(nIOPs - 3000) : 0;
				fp64 IOPSCost = m_GP3IOPSPricePerMonth * AdditionalIOPS;

				// Additional throughput cost (above 125 MB/s baseline)
				fp64 AdditionalThroughput = (nBandwidthMBps > 125) ? (fp64)(nBandwidthMBps - 125) : 0;
				fp64 ThroughputCost = m_GP3ThroughputPricePerMonth * AdditionalThroughput;

				App.m_StorageCostPerMonth = BaseCost + IOPSCost + ThroughputCost;
			}
			else
				App.m_StorageCostPerMonth = 0;

			// Calculate snapshot cost with GFS retention schedule (7 daily, 4 weekly, 12 monthly)
			// Incremental snapshots typically use ~1.5x volume size total (worst case estimate)
			if (App.f_GetHasSnapshot() && nStorageGB > 0)
				App.m_SnapshotCostPerMonth = m_SnapshotPricePerGBMonth * (fp64)nStorageGB * 1.5;
			else
				App.m_SnapshotCostPerMonth = 0;
		}

		// Count hosts and public service hosts for cost calculation
		TCVector<CStr> AllHosts;
		TCVector<CStr> PublicServiceHosts;
		for (auto const &App : m_Applications)
		{
			// Count all unique hosts
			bool bFoundAll = false;
			for (auto const &Host : AllHosts)
			{
				if (Host == App.m_Host)
				{
					bFoundAll = true;
					break;
				}
			}
			if (!bFoundAll)
				AllHosts.f_Insert(App.m_Host);

			// Count public service hosts (CloudManager or VersionManager)
			if (App.m_Type == EApplicationType::mc_CloudManager || App.m_Type == EApplicationType::mc_VersionManager)
			{
				bool bFoundService = false;
				for (auto const &Host : PublicServiceHosts)
				{
					if (Host == App.m_Host)
					{
						bFoundService = true;
						break;
					}
				}
				if (!bFoundService)
					PublicServiceHosts.f_Insert(App.m_Host);
			}
		}

		// Update other costs that depend on pricing and NAT configuration
		for (auto &Other : m_OtherCosts)
		{
			if (Other.m_Description.f_StartsWith("NAT Gateway") || Other.m_Description.f_StartsWith("Regional NAT"))
			{
				switch (m_NATConfiguration)
				{
				case ENATConfiguration::mc_None:
					// Should not have NAT entry
					break;

				case ENATConfiguration::mc_Single:
					{
						fp64 HourlyCost = m_NATGatewayPricePerHour * gc_HoursPerMonth;
						fp64 DataCost = m_NATGatewayDataProcessedPerGB * (fp64)m_NATDataTransferGBPerMonth;
						Other.m_Description = "NAT Gateway ({} GB)"_f << m_NATDataTransferGBPerMonth;
						Other.m_MonthlyCost = HourlyCost + DataCost;
						break;
					}

				case ENATConfiguration::mc_Regional:
					{
						fp64 RegionalHourlyRate = (m_RegionalNATGatewayPricePerHour > 0)
							? m_RegionalNATGatewayPricePerHour
							: m_NATGatewayPricePerHour;
						fp64 RegionalDataRate = (m_RegionalNATGatewayDataProcessedPerGB > 0)
							? m_RegionalNATGatewayDataProcessedPerGB
							: m_NATGatewayDataProcessedPerGB;
						fp64 HourlyCost = RegionalHourlyRate * gc_HoursPerMonth * (fp64)f_GetAvailabilityZoneCount();
						fp64 DataCost = RegionalDataRate * (fp64)m_NATDataTransferGBPerMonth;
						Other.m_Description = "Regional NAT ({} AZs, {} GB)"_f << f_GetAvailabilityZoneCount() << m_NATDataTransferGBPerMonth;
						Other.m_MonthlyCost = HourlyCost + DataCost;
						break;
					}

				case ENATConfiguration::mc_PerAZ:
					{
						fp64 HourlyCost = m_NATGatewayPricePerHour * gc_HoursPerMonth * (fp64)f_GetAvailabilityZoneCount();
						fp64 DataCost = m_NATGatewayDataProcessedPerGB * (fp64)m_NATDataTransferGBPerMonth;
						Other.m_Description = "NAT Gateway ({} AZs, {} GB)"_f << f_GetAvailabilityZoneCount() << m_NATDataTransferGBPerMonth;
						Other.m_MonthlyCost = HourlyCost + DataCost;
						break;
					}
				}
			}
			else if (Other.m_Description.f_StartsWith("Public IPv4"))
			{
				mint nPublicIPs = 0;
				mint nPublicServiceHosts = PublicServiceHosts.f_GetLen();
				switch (m_NATConfiguration)
				{
				case ENATConfiguration::mc_None:
					nPublicIPs = AllHosts.f_GetLen();
					break;
				case ENATConfiguration::mc_Single:
					// Single NAT uses 1 IP for the NAT + service hosts
					nPublicIPs = 1 + nPublicServiceHosts;
					break;
				case ENATConfiguration::mc_Regional:
					// Regional NAT uses 1 IP for the NAT + service hosts
					nPublicIPs = 1 + nPublicServiceHosts;
					break;
				case ENATConfiguration::mc_PerAZ:
					// Per-AZ NAT uses 1 IP per AZ + service hosts
					nPublicIPs = (mint)f_GetAvailabilityZoneCount() + nPublicServiceHosts;
					break;
				}
				Other.m_Description = "Public IPv4 ({} addresses)"_f << nPublicIPs;
				Other.m_MonthlyCost = m_PublicIPPricePerHour * gc_HoursPerMonth * (fp64)nPublicIPs;
			}
		}
	}

	CStr CBootstrapConfig::fs_GetIsolationLevelDisplayName(EIsolationLevel _Level)
	{
		switch (_Level)
		{
		case EIsolationLevel::mc_Maximum:
			return "Maximum";
		case EIsolationLevel::mc_Moderate:
			return "Moderate";
		case EIsolationLevel::mc_Minimal:
			return "Minimal";
		}
		return "Unknown";
	}

	CStr CBootstrapConfig::fs_GetStorageIsolationDisplayName(EStorageIsolation _Isolation)
	{
		switch (_Isolation)
		{
		case EStorageIsolation::mc_Shared:
			return "Shared";
		case EStorageIsolation::mc_Separate:
			return "Separate";
		case EStorageIsolation::mc_SeparateAll:
			return "Separate (All)";
		}
		return "Unknown";
	}

	CStr CBootstrapConfig::fs_GetSnapshotLevelDisplayName(ESnapshotLevel _Level)
	{
		switch (_Level)
		{
		case ESnapshotLevel::mc_None:
			return "None";
		case ESnapshotLevel::mc_Critical:
			return "Critical";
		case ESnapshotLevel::mc_All:
			return "All";
		}
		return "Unknown";
	}

	CStr CBootstrapConfig::fs_GetNATConfigurationDisplayName(ENATConfiguration _Config)
	{
		switch (_Config)
		{
		case ENATConfiguration::mc_None:
			return "None";
		case ENATConfiguration::mc_Single:
			return "Single";
		case ENATConfiguration::mc_Regional:
			return "Regional";
		case ENATConfiguration::mc_PerAZ:
			return "Per AZ";
		}
		return "Unknown";
	}

	CStr CPricingSelection::f_GetReservedPriceKey() const
	{
		CStr Term = (m_Term == EReservedTerm::mc_OneYear) ? "1yr" : "3yr";
		CStr Class = (m_OfferingClass == EOfferingClass::mc_Standard) ? "standard" : "convertible";
		CStr Payment;
		switch (m_Payment)
		{
		case EPaymentOption::mc_NoUpfront:
			Payment = "No Upfront";
			break;
		case EPaymentOption::mc_PartialUpfront:
			Payment = "Partial Upfront";
			break;
		case EPaymentOption::mc_AllUpfront:
			Payment = "All Upfront";
			break;
		}
		return "{}|{}|{}"_f << Term << Class << Payment;
	}

	CStr CPricingSelection::fs_GetDisplayName(CPricingSelection const &_Selection)
	{
		if (_Selection.f_IsOnDemand())
			return "On-Demand";

		CStr Term = (_Selection.m_Term == EReservedTerm::mc_OneYear) ? "1yr" : "3yr";
		CStr Class = (_Selection.m_OfferingClass == EOfferingClass::mc_Standard) ? "Standard" : "Convertible";
		CStr Payment;
		switch (_Selection.m_Payment)
		{
		case EPaymentOption::mc_NoUpfront:
			Payment = "NoUpfront";
			break;
		case EPaymentOption::mc_PartialUpfront:
			Payment = "Partial";
			break;
		case EPaymentOption::mc_AllUpfront:
			Payment = "AllUpfront";
			break;
		}
		return "{} {} {}"_f << Term << Class << Payment;
	}

	CStr CPricingSelection::fs_GetShortName(CPricingSelection const &_Selection)
	{
		if (_Selection.f_IsOnDemand())
			return "On-Demand";

		CStr Term = (_Selection.m_Term == EReservedTerm::mc_OneYear) ? "1Y" : "3Y";
		CStr Class = (_Selection.m_OfferingClass == EOfferingClass::mc_Standard) ? "Std" : "Conv";
		CStr Payment;
		switch (_Selection.m_Payment)
		{
		case EPaymentOption::mc_NoUpfront:
			Payment = "NoUp";
			break;
		case EPaymentOption::mc_PartialUpfront:
			Payment = "Part";
			break;
		case EPaymentOption::mc_AllUpfront:
			Payment = "AllUp";
			break;
		}
		return "{} {} {}"_f << Term << Class << Payment;
	}

	fp64 CBootstrapConfig::f_GetEffectiveInstancePrice(CStr const &_InstanceType) const
	{
		if (m_Pricing.f_IsOnDemand())
		{
			if (auto *pPrice = m_InstancePrices.f_FindEqual(_InstanceType))
				return *pPrice;
			return 0;
		}

		CStr Key = "{}|{}"_f << _InstanceType << m_Pricing.f_GetReservedPriceKey();
		if (auto *pPrice = m_ReservedPrices.f_FindEqual(Key))
			return *pPrice;

		// Fallback to on-demand if reserved price not found
		if (auto *pPrice = m_InstancePrices.f_FindEqual(_InstanceType))
			return *pPrice;
		return 0;
	}

	TCVector<CMatchingInstance> CBootstrapConfig::f_GetMatchingInstances(fp64 _RequiredMemoryGB) const
	{
		TCVector<CMatchingInstance> Entries;

		for (auto const &Entry : m_InstanceInfo.f_Entries())
		{
			CStr const &InstanceType = Entry.f_Key();
			CInstanceInfo const &Info = Entry.f_Value();

			// Check memory requirement
			if (Info.m_MemoryGB < _RequiredMemoryGB)
				continue;

			// Check CPU architecture and generation preference
			switch (m_CPUType)
			{
			case ECPUType::mc_X64:
				if (Info.m_Architecture != ECPUArchitecture::mc_X64)
					continue;
				break;
			case ECPUType::mc_X64CurrentGen:
				if (Info.m_Architecture != ECPUArchitecture::mc_X64 || !Info.m_bCurrentGeneration)
					continue;
				break;
			case ECPUType::mc_Arm64:
				if (Info.m_Architecture != ECPUArchitecture::mc_Arm64)
					continue;
				break;
			case ECPUType::mc_Arm64CurrentGen:
				if (Info.m_Architecture != ECPUArchitecture::mc_Arm64 || !Info.m_bCurrentGeneration)
					continue;
				break;
			case ECPUType::mc_AnyCurrentGen:
				if (!Info.m_bCurrentGeneration)
					continue;
				break;
			case ECPUType::mc_Any:
				// Accept any architecture and generation
				break;
			}

			// Get effective price (respects current pricing selection)
			fp64 Price = f_GetEffectiveInstancePrice(InstanceType);
			if (Price <= 0)
				continue;	// No pricing available

			CMatchingInstance &NewEntry = Entries.f_Insert();
			NewEntry.m_InstanceType = InstanceType;
			NewEntry.m_Price = Price;
		}

		// Sort by price ascending
		Entries.f_Sort([](CMatchingInstance const &_A, CMatchingInstance const &_B)
			{
				return _A.m_Price <=> _B.m_Price;
			}
		);

		return Entries;
	}

	CStr CBootstrapConfig::f_FindCheapestInstance(fp64 _RequiredMemoryGB) const
	{
		auto Matches = f_GetMatchingInstances(_RequiredMemoryGB);
		if (Matches.f_IsEmpty())
			return {};
		return Matches[0].m_InstanceType;
	}

	CStr CBootstrapConfig::fs_GetCPUTypeDisplayName(ECPUType _Type)
	{
		switch (_Type)
		{
		case ECPUType::mc_Any:
			return "Any";
		case ECPUType::mc_AnyCurrentGen:
			return "Any (Current Gen)";
		case ECPUType::mc_X64:
			return "x64";
		case ECPUType::mc_X64CurrentGen:
			return "x64 (Current Gen)";
		case ECPUType::mc_Arm64:
			return "ARM64";
		case ECPUType::mc_Arm64CurrentGen:
			return "ARM64 (Current Gen)";
		}
		return "Unknown";
	}

	CStr CBootstrapConfig::fs_GetArchitectureDisplayName(ECPUArchitecture _Arch)
	{
		switch (_Arch)
		{
		case ECPUArchitecture::mc_X64:
			return "x64";
		case ECPUArchitecture::mc_Arm64:
			return "ARM64";
		case ECPUArchitecture::mc_Unknown:
			return "-";
		}
		return "-";
	}

	bool CBootstrapConfig::f_HasAnyCustomizations() const
	{
		for (auto const &App : m_Applications)
		{
			if (App.f_HasCustomization())
				return true;
		}
		return false;
	}

	void CBootstrapConfig::f_ClearAllCustomizations()
	{
		for (auto &App : m_Applications)
			App.f_ClearCustomizations();
	}

	void CBootstrapConfig::f_ApplyPricing(CAwsPricingData &&_Pricing)
	{
		m_InstancePrices = fg_Move(_Pricing.m_InstancePrices);
		m_ReservedPrices = fg_Move(_Pricing.m_ReservedPrices);
		m_InstanceInfo = fg_Move(_Pricing.m_InstanceInfo);
		m_EBSPricePerGBMonth = _Pricing.m_EBSPricePerGBMonth;
		m_GP3IOPSPricePerMonth = _Pricing.m_GP3IOPSPricePerMonth;
		m_GP3ThroughputPricePerMonth = _Pricing.m_GP3ThroughputPricePerMonth;
		m_SnapshotPricePerGBMonth = _Pricing.m_SnapshotPricePerGBMonth;
		m_PublicIPPricePerHour = _Pricing.m_PublicIPPricePerHour;
		m_NATGatewayPricePerHour = _Pricing.m_NATGatewayPricePerHour;
		m_RegionalNATGatewayPricePerHour = _Pricing.m_RegionalNATGatewayPricePerHour;
		m_NATGatewayDataProcessedPerGB = _Pricing.m_NATGatewayDataProcessedPerGB;
		m_RegionalNATGatewayDataProcessedPerGB = _Pricing.m_RegionalNATGatewayDataProcessedPerGB;
	}
}
