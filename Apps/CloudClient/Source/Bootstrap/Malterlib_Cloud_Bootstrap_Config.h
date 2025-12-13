// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Container/Vector>
#include <Mib/Container/Map>
#include <Mib/Container/Set>
#include <Mib/Storage/Optional>

namespace NMib::NCloud::NBootstrap
{
	struct CAwsPricingData;

	// Isolation level options (from AWS_Infrastructure_Setup.md)
	enum class EIsolationLevel : uint32
	{
		mc_Minimal		// Option 1: All core apps on one host - lowest cost
		, mc_Moderate	// Option 2: KeyManagers isolated, management apps consolidated
		, mc_Maximum	// Option 3: One app per host - strongest security boundaries
	};

	// Storage isolation options
	enum class EStorageIsolation : uint32
	{
		mc_Shared			// Apps share host's root volume (KeyManagers use no extra storage)
		, mc_Separate		// Management apps get dedicated EBS volumes, KeyManagers stay on root
		, mc_SeparateAll	// All apps including KeyManagers get dedicated EBS volumes
	};

	// Snapshot level options
	enum class ESnapshotLevel : uint32
	{
		mc_None			// No snapshots
		, mc_Critical	// Only critical data (KeyManager, SecretsManager)
		, mc_All		// All volumes
	};

	// NAT configuration options
	enum class ENATConfiguration : uint32
	{
		mc_None			// No NAT - all hosts get public IPs directly
		, mc_Single		// Single NAT Gateway in one AZ (lowest cost with NAT)
		, mc_Regional	// Regional NAT Gateway spanning all AZs (single public IP)
		, mc_PerAZ		// One NAT Gateway per availability zone (each with own public IP)
	};

	// Reserved Instance term options
	enum class EReservedTerm : uint32
	{
		mc_OnDemand		// No reservation - use on-demand pricing
		, mc_OneYear	// 1-year commitment
		, mc_ThreeYear	// 3-year commitment
	};

	// Reserved Instance offering class
	enum class EOfferingClass : uint32
	{
		mc_Standard			// Standard RI - higher discount, less flexibility
		, mc_Convertible	// Convertible RI - can change instance family
	};

	// Payment options
	enum class EPaymentOption : uint32
	{
		mc_NoUpfront
		, mc_PartialUpfront
		, mc_AllUpfront
	};

	// CPU architecture preference
	enum class ECPUType : uint32
	{
		mc_Any					// Auto-select cheapest (any generation)
		, mc_AnyCurrentGen		// Auto-select cheapest (current generation only)
		, mc_X64				// Intel/AMD x86_64 only (any generation)
		, mc_X64CurrentGen		// Intel/AMD x86_64 only (current generation only)
		, mc_Arm64				// AWS Graviton (ARM64) only (any generation)
		, mc_Arm64CurrentGen	// AWS Graviton (ARM64) only (current generation only)
	};

	// CPU architecture classification (derived from PhysicalProcessor)
	enum class ECPUArchitecture : uint32
	{
		mc_Unknown
		, mc_X64	// Intel or AMD processors
		, mc_Arm64	// AWS Graviton processors
	};

	// Package source type for bootstrap
	enum class EPackageSource : uint32
	{
		mc_LocalDirectory	// Use existing packages from local directory
		, mc_VersionManager	// Download from connected VersionManager
	};

	// Combined pricing selection
	struct CPricingSelection
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream)
		{
			_Stream % m_Term;
			_Stream % m_OfferingClass;
			_Stream % m_Payment;
		}

		EReservedTerm m_Term = EReservedTerm::mc_OneYear;
		EOfferingClass m_OfferingClass = EOfferingClass::mc_Convertible;
		EPaymentOption m_Payment = EPaymentOption::mc_NoUpfront;

		bool f_IsOnDemand() const
		{
			 return m_Term == EReservedTerm::mc_OnDemand;
		}

		NStr::CStr f_GetReservedPriceKey() const;	// Returns "1yr|standard|No Upfront" format
		static NStr::CStr fs_GetDisplayName(CPricingSelection const &_Selection);
		static NStr::CStr fs_GetShortName(CPricingSelection const &_Selection);
	};

	// Application types in the deployment
	enum class EApplicationType : uint32
	{
		mc_KeyManager
		, mc_CloudManager
		, mc_VersionManager
		, mc_SecretsManager
		, mc_AppManager // Implicit - runs on each host
	};

	// Instance metadata from AWS Pricing API
	struct CInstanceInfo
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream)
		{
			_Stream % m_VCPUs;
			_Stream % m_MemoryGB;
			_Stream % m_Architecture;
			_Stream % m_bCurrentGeneration;
		}

		uint32 m_VCPUs = 0;
		fp64 m_MemoryGB = 0;
		ECPUArchitecture m_Architecture = ECPUArchitecture::mc_Unknown;
		bool m_bCurrentGeneration = false;
	};

	// Application entry in the deployment table
	struct CDeploymentApplication
	{
		// Stream only user-customizable fields for config persistence
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream)
		{
			_Stream % m_Name;
			_Stream % m_Type;
			_Stream % m_InstanceTypeOverride;
			_Stream % m_StorageGBOverride;
			_Stream % m_BandwidthMBpsOverride;
			_Stream % m_IOPsOverride;
			_Stream % m_SnapshotOverride;
		}

		NStr::CStr m_Name;					// Display name (e.g., "KeyManager A")
		EApplicationType m_Type;			// Application type
		NStr::CStr m_Host;					// Host name (filled based on isolation)
		NStr::CStr m_InstanceType;			// EC2 instance type (e.g., "t3.small")
		uint32 m_VCPUs = 0;					// Number of vCPUs for instance type
		fp64 m_MemoryGB = 0;				// Memory in GB for instance type
		uint32 m_StorageGB = 0;				// Storage in GB (0 = shared with host owner)
		uint32 m_BandwidthMBps = 125;		// EBS bandwidth MB/s
		uint32 m_IOPs = 3000;				// EBS IOPS
		fp64 m_InstanceCostPerHour = 0;		// Fetched from AWS Pricing API
		fp64 m_StorageCostPerMonth = 0;		// Calculated from storage
		fp64 m_SnapshotCostPerMonth = 0;	// Calculated if snapshots enabled
		bool m_bOwnsRootVolume = false;		// True for the first app on a host (AppManager)
		bool m_bHasSnapshot = false;		// Whether to snapshot this volume
		bool m_bIsCritical = false;			// Critical apps: KeyManager, SecretsManager

		// Per-cell customization (overrides auto-calculated values)
		NStorage::TCOptional<NStr::CStr> m_InstanceTypeOverride;
		NStorage::TCOptional<uint32> m_StorageGBOverride;
		NStorage::TCOptional<uint32> m_BandwidthMBpsOverride;
		NStorage::TCOptional<uint32> m_IOPsOverride;
		NStorage::TCOptional<bool> m_SnapshotOverride;

		// Check if this application has any customizations
		bool f_HasCustomization() const
		{
			return m_InstanceTypeOverride || m_StorageGBOverride || m_BandwidthMBpsOverride || m_IOPsOverride || m_SnapshotOverride;
		}

		// Clear all customizations
		void f_ClearCustomizations()
		{
			m_InstanceTypeOverride.f_Clear();
			m_StorageGBOverride.f_Clear();
			m_BandwidthMBpsOverride.f_Clear();
			m_IOPsOverride.f_Clear();
			m_SnapshotOverride.f_Clear();
		}

		// Get effective instance type (override or default)
		NStr::CStr const &f_GetInstanceType() const
		{
			return m_InstanceTypeOverride ? *m_InstanceTypeOverride : m_InstanceType;
		}

		// Get effective storage in GB (override or default)
		uint32 f_GetStorageGB() const
		{
			return m_StorageGBOverride ? *m_StorageGBOverride : m_StorageGB;
		}

		// Get effective bandwidth in MB/s (override or default)
		uint32 f_GetBandwidthMBps() const
		{
			return m_BandwidthMBpsOverride ? *m_BandwidthMBpsOverride : m_BandwidthMBps;
		}

		// Get effective IOPS (override or default)
		uint32 f_GetIOPs() const
		{
			return m_IOPsOverride ? *m_IOPsOverride : m_IOPs;
		}

		// Get effective snapshot flag (override or default)
		bool f_GetHasSnapshot() const
		{
			return m_SnapshotOverride ? *m_SnapshotOverride : m_bHasSnapshot;
		}
	};

	// Other cost items (NAT Gateway, Public IPs, S3, etc.)
	struct COtherCost
	{
		NStr::CStr m_Description;
		fp64 m_MonthlyCost = 0;
	};

	// Entry for matching instance results
	struct CMatchingInstance
	{
		NStr::CStr m_InstanceType;
		fp64 m_Price;
	};

	// Package configuration state for bootstrap
	struct CPackageConfig
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream)
		{
			_Stream % m_Source;
			_Stream % m_LocalPackageDirectory;
			_Stream % m_SelectedApplications;
			_Stream % m_SelectedPlatforms;
			_Stream % m_Branch;
			_Stream % m_Tag;
		}

		EPackageSource m_Source = EPackageSource::mc_VersionManager;
		NStr::CStr m_LocalPackageDirectory;					// For mc_LocalDirectory source
		NContainer::TCMap<NStr::CStr, bool> m_SelectedApplications;	// App name -> selected
		NContainer::TCSet<NStr::CStr> m_SelectedPlatforms;			// Selected platforms
		NStr::CStr m_Branch;	// e.g., "master/" - empty for any branch
		NStr::CStr m_Tag;		// e.g., "Production" - empty for any tag
	};

	// Main configuration state
	struct CBootstrapConfig
	{
		enum : uint32
		{
			EVersion_Initial = 1,
			EVersion_PackageConfig = 2,		// Added package configuration
			EVersion_Current = EVersion_PackageConfig
		};

		// Stream user settings and application customizations for persistence
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream)
		{
			uint32 Version = EVersion_Current;
			_Stream % Version;

			if (Version > EVersion_Current)
				DMibErrorInstance("Unsupported config version");

			// User settings
			_Stream % m_Region;
			_Stream % m_IsolationLevel;
			_Stream % m_StorageIsolation;
			_Stream % m_bStorageEncryption;
			_Stream % m_SnapshotLevel;
			_Stream % m_NATConfiguration;
			_Stream % m_NATDataTransferGBPerMonth;
			_Stream % m_KeyManagerCount;
			_Stream % m_CPUType;
			_Stream % m_Pricing;

			// Application customizations (name, type, and overrides only)
			_Stream % m_Applications;

			// Package configuration (added in EVersion_PackageConfig)
			if (Version >= EVersion_PackageConfig)
				_Stream % m_PackageConfig;
		}

		NStr::CStr m_Region;
		EIsolationLevel m_IsolationLevel = EIsolationLevel::mc_Maximum;
		EStorageIsolation m_StorageIsolation = EStorageIsolation::mc_SeparateAll;
		bool m_bStorageEncryption = true;
		ESnapshotLevel m_SnapshotLevel = ESnapshotLevel::mc_All;
		ENATConfiguration m_NATConfiguration = ENATConfiguration::mc_Regional;
		uint32 m_NATDataTransferGBPerMonth = 100; // Estimated NAT data transfer in GB/month
		uint32 m_KeyManagerCount = 3; // 1, 2, or 3
		ECPUType m_CPUType = ECPUType::mc_AnyCurrentGen; // CPU architecture preference
		CPricingSelection m_Pricing; // Reserved Instance pricing selection

		NContainer::TCVector<CDeploymentApplication> m_Applications;
		NContainer::TCVector<COtherCost> m_OtherCosts;
		CPackageConfig m_PackageConfig;	// Package source and selection

		// Pricing cache (instance type -> hourly price)
		NContainer::TCMap<NStr::CStr, fp64> m_InstancePrices;
		// Reserved prices: Key = "{InstanceType}|{Term}|{Class}|{Payment}"
		NContainer::TCMap<NStr::CStr, fp64> m_ReservedPrices;
		// Instance metadata (vCPUs, memory)
		NContainer::TCMap<NStr::CStr, CInstanceInfo> m_InstanceInfo;
		fp64 m_EBSPricePerGBMonth = 0;						// gp3 price per GB/month
		fp64 m_GP3IOPSPricePerMonth = 0;					// gp3 price per IOPS/month (above 3000 baseline)
		fp64 m_GP3ThroughputPricePerMonth = 0;				// gp3 price per MB/s/month (above 125 baseline)
		fp64 m_SnapshotPricePerGBMonth = 0;					// Snapshot price per GB/month
		fp64 m_PublicIPPricePerHour = 0;					// Public IPv4 price per hour
		fp64 m_NATGatewayPricePerHour = 0;					// NAT Gateway price per hour (per-AZ type)
		fp64 m_RegionalNATGatewayPricePerHour = 0;			// Regional NAT Gateway price per hour (per active AZ)
		fp64 m_NATGatewayDataProcessedPerGB = 0;			// NAT Gateway data processing per GB
		fp64 m_RegionalNATGatewayDataProcessedPerGB = 0;	// Regional NAT Gateway data processing per GB

		// Get availability zone count (derived from KeyManager count since they deploy across AZs)
		uint32 f_GetAvailabilityZoneCount() const
		{
			 return m_KeyManagerCount;
		}

		// Calculate total monthly cost from all applications and other costs
		fp64 f_CalculateTotalMonthlyCost() const;

		// Rebuild m_Applications and m_OtherCosts based on current settings
		void f_RecalculateLayout();

		// Update costs for all applications based on cached prices
		void f_UpdateCosts();

		// Get effective hourly price for an instance type based on pricing selection
		fp64 f_GetEffectiveInstancePrice(NStr::CStr const &_InstanceType) const;

		// Find cheapest instance type meeting memory requirement and CPU preference
		NStr::CStr f_FindCheapestInstance(fp64 _RequiredMemoryGB) const;

		// Get all instance types meeting memory requirement and CPU preference, sorted by price
		NContainer::TCVector<CMatchingInstance> f_GetMatchingInstances(fp64 _RequiredMemoryGB) const;

		// Get display string for isolation level
		static NStr::CStr fs_GetIsolationLevelDisplayName(EIsolationLevel _Level);

		// Get display string for storage isolation
		static NStr::CStr fs_GetStorageIsolationDisplayName(EStorageIsolation _Isolation);

		// Get display string for snapshot level
		static NStr::CStr fs_GetSnapshotLevelDisplayName(ESnapshotLevel _Level);

		// Get display string for NAT configuration
		static NStr::CStr fs_GetNATConfigurationDisplayName(ENATConfiguration _Config);

		// Get display string for CPU type
		static NStr::CStr fs_GetCPUTypeDisplayName(ECPUType _Type);

		// Get display string for CPU architecture
		static NStr::CStr fs_GetArchitectureDisplayName(ECPUArchitecture _Arch);

		// Check if any applications have customizations
		bool f_HasAnyCustomizations() const;

		// Clear all customizations from all applications
		void f_ClearAllCustomizations();

		// Apply pricing data from AWS pricing fetch (forward declaration in ConfigUI.h)
		void f_ApplyPricing(CAwsPricingData &&_Pricing);
	};
}
