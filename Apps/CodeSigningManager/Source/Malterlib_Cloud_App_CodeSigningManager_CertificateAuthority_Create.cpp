// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_CodeSigningManager.h"

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NCodeSigningManager
{
	void CCodeSigningManagerActor::fp_Authority_StoreSecrets
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
	{
		CSecretsManager::CSecretID PublicSecretID;
		CSecretsManager::CSecretProperties PublicProperties;
		{
			PublicSecretID.m_Folder = mc_pAuthorityPublicFolder;
			PublicSecretID.m_Name = _Name;

			TCMap<CStrSecure, CStrSecure> PublicSecret;
			PublicSecret["Certificate"] = _Certificate.m_Certificate.f_ToString();

			PublicProperties.f_SetSecret(PublicSecret);
			PublicProperties.f_SetSemanticID(CStrSecure::CFormat(CStr(mc_pAuthoritySemanticPrefix) + "{}") << _Name);
			PublicProperties.f_SetMetadata("Serial", _Serial);
			PublicProperties.f_SetMetadata("KeyType", fsp_PublicKeySettingToStr(_PublicKeySetting));
			PublicProperties.f_SetTags(TCSet<CStrSecure>{"Public"});

			PublicProperties.m_Created = _Created;
			PublicProperties.m_Modified = _Modified;
			PublicProperties.m_Immutable = true;
		}

		CSecretsManager::CSecretID PrivateSecretID;
		CSecretsManager::CSecretProperties PrivateProperties;
		{
			PrivateSecretID.m_Folder = mc_pAuthorityPrivateFolder;
			PrivateSecretID.m_Name = _Name;

			TCMap<CStrSecure, CStrSecure> PrivateSecret;
			PrivateSecret["PrivateKey"] = _Certificate.m_Key.f_ToString();

			PrivateProperties.f_SetSecret(PrivateSecret);
			PrivateProperties.f_SetSemanticID(CStrSecure::CFormat(CStr(mc_pAuthoritySemanticPrefix) + "{}") << _Name);
			PrivateProperties.f_SetMetadata("KeyType", fsp_PublicKeySettingToStr(_PublicKeySetting));
			PrivateProperties.f_SetTags(TCSet<CStrSecure>{"Private"});

			PrivateProperties.m_Created = _Created;
			PrivateProperties.m_Modified = _Modified;
			PrivateProperties.m_Immutable = true;
		}

		for (auto &SecretManager : _SecretManagers)
		{
			auto WeakSecretsManager = SecretManager.m_Actor.f_Weak();

			SecretManager.m_Actor.f_CallActor(&CSecretsManager::f_SetSecretProperties)(PublicSecretID, fg_TempCopy(PublicProperties))
				> o_PublicStoreResultsAsync[WeakSecretsManager]
			;

			SecretManager.m_Actor.f_CallActor(&CSecretsManager::f_SetSecretProperties)(PrivateSecretID, fg_TempCopy(PrivateProperties))
				> o_PrivateStoreResultsAsync[WeakSecretsManager]
			;
		}
	}

	TCFuture<uint32> CCodeSigningManagerActor::fp_CommandLine_AuthorityCreate(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto Auditor = f_Auditor();

		CPublicKeySetting PublicKeySetting;

		if (auto *pRSASize = _Params.f_GetMember("RSASize", EJsonType_Integer))
			PublicKeySetting = CPublicKeySettings_RSA(pRSASize->f_Integer());
		else
			PublicKeySetting = fsp_EllipticCurveTypeToKeySettings(fsp_EllipticCurveTypeFromStr(_Params["EllipticCurveType"].f_String()));

		CStr Name = _Params["Name"].f_String();

		if (!CAuthority::fs_IsValidName(Name))
			co_return Auditor.f_Exception("'{}' is not a valid certificate authority name"_f << Name);

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> NException::CExceptionPointer
				{
					if (mp_State.m_bStoppingApp || f_IsDestroyed())
						return DMibErrorInstance("Startup aborted");

					if (mp_SecretsManagerSubscription.m_Actors.f_IsEmpty())
						return DMibErrorInstance("No secret managers connected");

					return {};
				}
			)
		;

		CCertificateAndKey CaCertificate = co_await
			(
				g_ConcurrentDispatch / [PublicKeySetting, Name]
				{
					CByteVector CaCertData;
					CSecureByteVector CaKeyData;

					CCertificateSignOptions SignOptions;
					SignOptions.m_Days = 365*100;

					CCertificateOptions Options;
					Options.m_CommonName = "Malterlib CodeSign CA {} - {nfh,sj16,sf0}"_f <<  Name << fg_GetHighEntropyRandomInteger<uint64>();
					Options.m_KeySetting = PublicKeySetting;
					Options.f_MakeCA();
					SignOptions.f_AddExtension_SubjectKeyIdentifier();

					CCertificateAndKey Certificate;

					CCertificate::fs_GenerateSelfSignedCertAndKey
						(
							Options
							, Certificate.m_Certificate
							, Certificate.m_Key
							, SignOptions
						)
					;

					return Certificate;
				}
			)
		;

		NTime::CTime Now = NTime::CTime::fs_NowUTC();

		auto AllSecretManagers = fg_VectorFromContainer(mp_SecretsManagerSubscription.m_Actors);

		TCMap<TCWeakDistributedActor<CSecretsManager>, CTime> SecretManagers;
		TCMap<TCWeakDistributedActor<CSecretsManager>, CStr> SecretManagerDescriptions;

		for (auto &SecretManager : AllSecretManagers)
		{
			auto WeakSecretsManager = SecretManager.m_Actor.f_Weak();
			SecretManagers[WeakSecretsManager] = Now;
			SecretManagerDescriptions[WeakSecretsManager] = "{}"_f << SecretManager.m_TrustInfo.m_HostInfo;
		}

		if (mp_Authorities.f_FindEqual(Name))
			co_return Auditor.f_Exception("Certificate authority '{}' already exists"_f << Name);

		int32 Serial = 2; // CA certificate gets serial 1, so let's start the client certificates on serial 2
		{
			TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> PublicStoreResultsAsync;
			TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> PrivateStoreResultsAsync;
			fp_Authority_StoreSecrets
				(
					AllSecretManagers
					, Name
					, Serial
					, PublicKeySetting
					, Now
					, Now
					, CaCertificate
					, PublicStoreResultsAsync
					, PrivateStoreResultsAsync
				)
			;

			auto PublicResults = co_await fg_AllDoneWrapped(PublicStoreResultsAsync);
			auto PrivateResults = co_await fg_AllDoneWrapped(PrivateStoreResultsAsync);

			for (auto &SecretManager : AllSecretManagers)
			{
				auto WeakSecretsManager = SecretManager.m_Actor.f_Weak();
				auto *pPublicResult = PublicResults.f_FindEqual(WeakSecretsManager);
				auto *pPrivateResult = PrivateResults.f_FindEqual(WeakSecretsManager);

				if (!pPublicResult || !*pPublicResult)
				{
					SecretManagers.f_Remove(WeakSecretsManager);

					CStr ErrorDescription = "Failed to add certificate authority public secret to secrets manager '{}': {}"_f
						<< SecretManagerDescriptions[WeakSecretsManager]
						<< (pPublicResult ? pPublicResult->f_GetExceptionStr() : CStr("missing result"))
					;

					DMibLog(Error, "{}", ErrorDescription);
					*_pCommandLine %= "{}\n"_f << ErrorDescription;
					continue;
				}

				if (!pPrivateResult || !*pPrivateResult)
				{
					SecretManagers.f_Remove(WeakSecretsManager);

					CStr ErrorDescription = "Failed to add certificate authority private secret to secrets manager '{}': {}"_f
						<< SecretManagerDescriptions[WeakSecretsManager]
						<< (pPrivateResult ? pPrivateResult->f_GetExceptionStr() : CStr("missing result"))
					;

					DMibLog(Error, "{}", ErrorDescription);
					*_pCommandLine %= "{}\n"_f << ErrorDescription;
					continue;
				}
			}

			if (SecretManagers.f_IsEmpty())
			{
				*_pCommandLine %= "None of the secret managers were successful, authority not added.\n";

				co_return 1;
			}

			Auditor.f_Info("Added Certificate Authority '{}'"_f << Name);
		}

		co_return 0;
	}
}
