// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Cloud/CodeSigningManager>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Cryptography/Certificate>
#include <Mib/Cryptography/SignFiles>

namespace NMib::NCloud::NCodeSigningManager
{
	struct CCodeSigningManagerActor : public CDistributedAppActor
	{
		CCodeSigningManagerActor();
		~CCodeSigningManagerActor();

	private:
		using EStatusSeverity = CDistributedAppSensorReporter::EStatusSeverity;

		struct CStatus
		{
			CStr m_Description;
			EStatusSeverity m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Ok;
		};

		struct CCertificateAndKey
		{
			CByteVector m_Certificate;
			CSecureByteVector m_Key;
		};

		struct CAuthority
		{
			CStr const &f_GetName() const;
			CSecretsManager::CSecretID f_GetPublicSecretID() const;
			CSecretsManager::CSecretID f_GetPrivateSecretID() const;
			static bool fs_IsValidName(CStr const &_Name);

			// Stored
			CByteVector m_Certificate;
			CPublicKeySetting m_PublicKeySetting = CPublicKeySettings_EC_secp521r1{};
			int32 m_Serial = 2;
			CTime m_Created;
			CTime m_LastModified;

			// Temporary
			CStatus m_Status;
			TCMap<TCWeakDistributedActor<CSecretsManager>, CTime> m_SecretsManagers;
			CDistributedAppSensorReporter::CSensorReporter m_SensorReporter_Status;
			CDistributedAppSensorReporter::CSensorReporter m_SensorReporter_Expire;
			bool m_bSensorsRegistered = false;
		};

		struct CSigningCertKey
		{
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const
			{
				o_Str += typename tf_CStr::CFormat("{}/{}") << m_Authority << m_Name;
			}

			auto operator <=> (CSigningCertKey const &_Right) const = default;

			CSecretsManager::CSecretID f_GetSecretID() const;
			CStr f_GetSecretIDName() const;

			CStr m_Authority;
			CStr m_Name;
		};

		struct CSigningCert
		{
			CSigningCertKey const &f_GetKey() const
			{
				return TCMap<CSigningCertKey, CSigningCert>::fs_GetKey(*this);
			}
			CSecretsManager::CSecretID f_GetPublicSecretID() const;
			CSecretsManager::CSecretID f_GetPrivateSecretID() const;
			static bool fs_IsValidName(CStr const &_Name);

			// Stored
			CByteVector m_Certificate;
			CPublicKeySetting m_PublicKeySetting = CPublicKeySettings_EC_secp521r1{};
			CTime m_Created;
			CTime m_LastModified;

			// Temporary
			CStatus m_Status;
			TCMap<TCWeakDistributedActor<CSecretsManager>, CTime> m_SecretsManagers;
			CDistributedAppSensorReporter::CSensorReporter m_SensorReporter_Status;
			CDistributedAppSensorReporter::CSensorReporter m_SensorReporter_Expire;
			bool m_bSensorsRegistered = false;
		};

		struct CCodeSigningInterface : public NCloud::CCodeSigningManager
		{
			TCFuture<CSignFiles::CResult> f_SignFiles(CSignFiles _Params) override;

			DMibDelegatedActorImplementation(CCodeSigningManagerActor);
		};

		struct CSignSession
		{
			CStr const &f_GetSignID() const
			{
				return TCMap<CStr, CSignSession>::fs_GetKey(*this);
			}

			TCActor<NCloud::CFileTransferReceive> m_FileTransferReceive;
			CStr m_WorkDirectory;
			CStr m_InputDirectory;
		};

		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

		TCFuture<void> fp_Destroy() override;

		TCFuture<void> fp_StartApp(NEncoding::CEJsonSorted const _Params) override;
		TCFuture<void> fp_StopApp() override;
		TCFuture<void> fp_Publish();
		TCFuture<void> fp_RegisterSensors();

		TCFuture<uint32> fp_CommandLine_AuthorityCreate(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_AuthorityList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_AuthorityResync(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_AuthorityInfo(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		TCFuture<uint32> fp_CommandLine_SigningCertCreate(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SigningCertList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SigningCertResync(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SigningCertReissue(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		static EPublicKeyType fsp_EllipticCurveTypeFromStr(CStr const &_String);
		static CStr fsp_EllipticCurveTypeToStr(EPublicKeyType _Type);
		static CPublicKeySetting fsp_PublicKeySettingFromStr(CStr const &_String);
		static CStr fsp_PublicKeySettingToStr(CPublicKeySetting const &_PublicKeySetting);
		static CPublicKeySetting fsp_EllipticCurveTypeToKeySettings(EPublicKeyType _Type);

		TCFuture<void> fp_SecretsManagerAddedWithRetry(TCDistributedActor<CSecretsManager> _SecretsManager, CTrustedActorInfo _Info);

		TCFuture<void> fp_SecretsManagerAdded(TCDistributedActor<CSecretsManager> _SecretsManager, CTrustedActorInfo _Info);
		TCFuture<void> fp_SecretsManagerRemoved(TCWeakDistributedActor<CActor> _SecretsManager, CTrustedActorInfo _ActorInfo);

		TCFuture<CCertificateAndKey> fp_GenerateSigningCertCertificate(CStr _AuthorityName, CByteVector _AuthorityCertificate, CPublicKeySetting _PublicKeySetting, CStr _SigningCertName);

		TCFuture<void> fp_Authority_UpdateSensor(CStr _Authority);
		TCFuture<void> fp_Authority_UpdateSensors();
		TCFuture<void> fp_Authority_RegisterSensors(CStr _Authority);
		TCFuture<void> fp_Authority_UpdateStatusSensor(CStr _Authority, EStatusSeverity _Severity, CStr _Status);
		void fp_Authority_UpdateStatus(CAuthority &o_Authority, EStatusSeverity _Severity, CStr const &_Status);
		TCFuture<void> fp_Authority_Add(TCDistributedActor<CSecretsManager> _SecretsManager, CSecretsManager::CSecretID _SecretID);
		TCFuture<void> fp_Authority_SecretsManagerAdded(TCDistributedActor<CSecretsManager> _SecretsManager, CTrustedActorInfo _Info);
		void fp_Authority_StoreSecrets
			(
				TCVector<TCTrustedActor<CSecretsManager>> const &_SecretManagers
				, CStr const &_Name
				, int32 _Serial
				, CPublicKeySetting const &_PublicKeySetting
				, CTime const &_Created
				, CTime const &_Modified
				, CCertificateAndKey const &_Certificate
				, TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> &o_PublicStoreResultsAsync
				, TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> &o_PrivateStoreResultsAsync
			)
		;
		TCFuture<int32> fp_Authority_GetNewSerial(CStr _AuthorityName);
		TCFuture<CSecureByteVector> fp_Authority_FetchPrivateKey(CStr _AuthorityName);

		TCFuture<CSecureByteVector> fp_SigningCert_FetchPrivateKey(CSigningCertKey _SigningCertKey);
		TCFuture<void> fp_SigningCert_UpdateSensor(CSigningCertKey _SigningCertKey);
		TCFuture<void> fp_SigningCert_UpdateSensors();
		TCFuture<void> fp_SigningCert_RegisterSensors(CSigningCertKey _SigningCertKey);
		TCFuture<void> fp_SigningCert_UpdateStatusSensor(CSigningCertKey _SigningCertKey, EStatusSeverity _Severity, CStr _Status);
		void fp_SigningCert_UpdateStatus(CSigningCert &o_SigningCert, EStatusSeverity _Severity, CStr const &_Status);
		TCFuture<void> fp_SigningCert_Add(TCDistributedActor<CSecretsManager> _SecretsManager, CSecretsManager::CSecretID _SecretID);
		TCFuture<void> fp_SigningCert_SecretsManagerAdded(TCDistributedActor<CSecretsManager> _SecretsManager, CTrustedActorInfo _Info);
		void fp_SigningCert_StoreSecrets
			(
				TCVector<TCTrustedActor<CSecretsManager>> const &_SecretManagers
				, CAuthority const &_Authority
				, CSigningCertKey const &_Key
				, CPublicKeySetting const &_PublicKeySetting
				, CTime const &_Created
				, CTime const &_Modified
				, CCertificateAndKey const &_Certificate
				, TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> &o_PublicStoreResultsAsync
				, TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> &o_PrivateStoreResultsAsync
			)
		;

		static constexpr ch8 const *mc_pAuthoritySemanticPrefix = "org.malterlib.codesign.authority#";
		static constexpr ch8 const *mc_pAuthorityPublicFolder = "org.malterlib.codesign.authority.public";
		static constexpr ch8 const *mc_pAuthorityPrivateFolder = "org.malterlib.codesign.authority.private";

		static constexpr ch8 const *mc_pSigningCertSemanticPrefix = "org.malterlib.codesign.signingcert#";
		static constexpr ch8 const *mc_pSigningCertPublicFolder = "org.malterlib.codesign.signingcert.public";
		static constexpr ch8 const *mc_pSigningCertPrivateFolder = "org.malterlib.codesign.signingcert.private";

		TCMap<CStr, CAuthority> mp_Authorities;
		TCMap<CSigningCertKey, CSigningCert> mp_SigningCerts;
		TCMap<CStr, CSignSession> mp_SignSessions;

		TCDistributedActorInstance<CCodeSigningInterface> mp_CodeSigningInterface;

		TCTrustedActorSubscription<CSecretsManager> mp_SecretsManagerSubscription;
		TCMap<TCWeakDistributedActor<CSecretsManager>, CStr> mp_LastSecretsManagerError;
		TCSet<TCWeakDistributedActor<CSecretsManager>> mp_RetryingSecretsManagers;
		TCMap<TCWeakDistributedActor<CSecretsManager>, CActorSubscription> mp_SigningCertSubscriptions;
		TCMap<TCWeakDistributedActor<CSecretsManager>, CActorSubscription> mp_AuthoritySubscriptions;

		CSequencer mp_InitSensorReporterSequencer{"CodeSigningManagerActor InitSensorReporterSequencer"};
		CActorSubscription mp_SensorUpdateTimerSubscription;

		NCryptography::CTimestampOptions mp_TimestampOptions;
	};
}
