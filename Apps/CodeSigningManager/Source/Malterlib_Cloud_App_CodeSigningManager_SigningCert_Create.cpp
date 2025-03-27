// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_CodeSigningManager.h"

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NCodeSigningManager
{
	bool CCodeSigningManagerActor::CAuthority::fs_IsValidName(CStr const &_Name)
	{
		return fg_IsValidHostname(_Name);
	}

	CStr CCodeSigningManagerActor::CSigningCertKey::f_GetSecretIDName() const
	{
		return "{}#{}"_f << m_Authority << m_Name;
	}

	CSecretsManager::CSecretID CCodeSigningManagerActor::CSigningCertKey::f_GetSecretID() const
	{
		CSecretsManager::CSecretID SecretID;
		SecretID.m_Folder = mc_pSigningCertPublicFolder;
		SecretID.m_Name = f_GetSecretIDName();

		return SecretID;
	}

	void CCodeSigningManagerActor::fp_SigningCert_StoreSecrets
		(
			TCVector<TCTrustedActor<CSecretsManager>> const &_SecretManagers
			, CAuthority const &_Authority
			, CSigningCertKey const &_SigningCertKey
			, CPublicKeySetting const &_PublicKeySetting
			, CTime const &_Created
			, CTime const &_Modified
			, CCertificateAndKey const &_Certificate
			, TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> &o_PublicStoreResultsAsync
			, TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> &o_PrivateStoreResultsAsync
		)
	{
		CSecretsManager::CSecretProperties PublicProperties;
		CSecretsManager::CSecretID PublicSecretID;
		{
			PublicSecretID.m_Folder = mc_pSigningCertPublicFolder;
			PublicSecretID.m_Name = _SigningCertKey.f_GetSecretIDName();

			TCMap<CStrSecure, CStrSecure> Secret;
			Secret["Certificate"] = _Certificate.m_Certificate.f_ToString();
			Secret["CA"] = _Authority.m_Certificate.f_ToString();

			PublicProperties.f_SetSecret(Secret);
			PublicProperties.f_SetSemanticID(CStrSecure::CFormat(CStr(mc_pSigningCertSemanticPrefix) + "{}") << _SigningCertKey.f_GetSecretIDName());
			PublicProperties.f_SetMetadata("KeyType", fsp_PublicKeySettingToStr(_PublicKeySetting));
			PublicProperties.f_SetTags(TCSet<CStrSecure>{"Public"});

			PublicProperties.m_Created = _Created;
			PublicProperties.m_Modified = _Modified;
		}

		CSecretsManager::CSecretProperties PrivateProperties;
		CSecretsManager::CSecretID PrivateSecretID;
		{
			PrivateSecretID.m_Folder = mc_pSigningCertPrivateFolder;
			PrivateSecretID.m_Name = _SigningCertKey.f_GetSecretIDName();

			TCMap<CStrSecure, CStrSecure> Secret;
			Secret["PrivateKey"] = _Certificate.m_Key.f_ToString();

			PrivateProperties.f_SetSecret(Secret);
			PrivateProperties.f_SetSemanticID(CStrSecure::CFormat(CStr(mc_pSigningCertSemanticPrefix) + "{}") << _SigningCertKey.f_GetSecretIDName());
			PrivateProperties.f_SetMetadata("KeyType", fsp_PublicKeySettingToStr(_PublicKeySetting));
			PrivateProperties.f_SetTags(TCSet<CStrSecure>{"Private"});

			PrivateProperties.m_Created = _Created;
			PrivateProperties.m_Modified = _Modified;
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

	auto CCodeSigningManagerActor::fp_GenerateSigningCertCertificate(CStr _AuthorityName, CByteVector _AuthorityCertificate, CPublicKeySetting _PublicKeySetting, CStr _SigningCertName)
		-> TCFuture<CCertificateAndKey>
	{
		CCertificateAndKey AuthorityCertificate;
		AuthorityCertificate.m_Certificate = fg_Move(_AuthorityCertificate);
		AuthorityCertificate.m_Key = co_await fp_Authority_FetchPrivateKey(fg_Move(_AuthorityName));

		co_return co_await
			(
				g_ConcurrentDispatch / [AuthorityCertificate = fg_Move(AuthorityCertificate), _PublicKeySetting, _SigningCertName]
				{
					TCMap<CStr, CStr> RelativeDistinguishedNames;

					RelativeDistinguishedNames["OU"] = "codesign.signingcert";
					RelativeDistinguishedNames["O"] = "malterlib.org";

					CCertificateSignOptions SignOptions;
					SignOptions.m_Serial = fg_GetRandom();
					SignOptions.m_Days = 365*10;
					SignOptions.f_AddExtension_AuthorityKeyIdentifier();

					CCertificateOptions Options;
					Options.m_CommonName = _SigningCertName;
					Options.m_RelativeDistinguishedNames = RelativeDistinguishedNames;
					Options.m_KeySetting = _PublicKeySetting;

					Options.f_AddExtension_BasicConstraints(false);
					Options.f_AddExtension_KeyUsage(EKeyUsage_DigitalSignature);
					Options.f_AddExtension_ExtendedKeyUsage(EExtendedKeyUsage_CodeSigning);

					CCertificateAndKey SigningCertCertificate;

					CByteVector CertRequestData;

					CCertificate::fs_GenerateClientCertificateRequest(Options, CertRequestData, SigningCertCertificate.m_Key);

					CCertificate::fs_SignClientCertificate
						(
							AuthorityCertificate.m_Certificate
							, AuthorityCertificate.m_Key
							, CertRequestData
							, SigningCertCertificate.m_Certificate
							, SignOptions
						)
					;

					return SigningCertCertificate;
				}
			)
		;
	}

	TCFuture<uint32> CCodeSigningManagerActor::fp_CommandLine_SigningCertCreate(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto Auditor = f_Auditor();

		CPublicKeySetting PublicKeySetting;

		if (auto *pRSASize = _Params.f_GetMember("RSASize", EJsonType_Integer))
			PublicKeySetting = CPublicKeySettings_RSA(pRSASize->f_Integer());
		else
			PublicKeySetting = fsp_EllipticCurveTypeToKeySettings(fsp_EllipticCurveTypeFromStr(_Params["EllipticCurveType"].f_String()));

		CStr SigningCertName = _Params["SigningCert"].f_String();
		CStr Authority = _Params["Authority"].f_String();

		if (!CAuthority::fs_IsValidName(Authority))
			co_return Auditor.f_Exception("'{}' is not a valid certificate authority name"_f << Authority);

		if (!CSigningCert::fs_IsValidName(SigningCertName))
			co_return Auditor.f_Exception("'{}' is not a valid signing certificate name"_f << SigningCertName);

		CSigningCertKey SigningCertKey;
		SigningCertKey.m_Authority = Authority;
		SigningCertKey.m_Name = SigningCertName;

		CAuthority *pAuthority = nullptr;

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> NException::CExceptionPointer
				{
					if (mp_State.m_bStoppingApp || f_IsDestroyed())
						return DMibErrorInstance("Startup aborted");

					if (mp_SecretsManagerSubscription.m_Actors.f_IsEmpty())
						return DMibErrorInstance("No secret managers connected");

					pAuthority = mp_Authorities.f_FindEqual(Authority);

					if (!pAuthority)
						return DMibErrorInstance("No such authority: '{}'"_f << Authority);

					return {};
				}
			)
		;

		CStr AuthorityNameCopy = pAuthority->f_GetName();
		CByteVector AuthorityCertificateData = pAuthority->m_Certificate;
		auto SigningCertCertificate = co_await fp_GenerateSigningCertCertificate(fg_Move(AuthorityNameCopy), fg_Move(AuthorityCertificateData), PublicKeySetting, SigningCertName);

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

		if (mp_SigningCerts.f_FindEqual(SigningCertKey))
			co_return Auditor.f_Exception("Signing certificate '{}' already exists"_f << SigningCertName);

		{
			TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> PublicStoreResultsAsync;
			TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> PrivateStoreResultsAsync;
			fp_SigningCert_StoreSecrets
				(
					AllSecretManagers
					, *pAuthority
					, SigningCertKey
					, PublicKeySetting
					, Now
					, Now
					, SigningCertCertificate
					, PublicStoreResultsAsync
					, PrivateStoreResultsAsync
				)
			;

			auto PublicStoreResults = co_await fg_AllDoneWrapped(PublicStoreResultsAsync);
			auto PrivateStoreResults = co_await fg_AllDoneWrapped(PrivateStoreResultsAsync);

			for (auto &StoreResult : PublicStoreResults)
			{
				auto &WeakSecretsManager = PublicStoreResults.fs_GetKey(StoreResult);

				if (!StoreResult)
				{
					SecretManagers.f_Remove(WeakSecretsManager);

					CStr ErrorDescription = "Failed to add signing certificate public secret to secrets manager '{}': {}"_f
						<< SecretManagerDescriptions[WeakSecretsManager]
						<< StoreResult.f_GetExceptionStr()
					;

					DMibLog(Error, "{}", ErrorDescription);
					*_pCommandLine %= "{}\n"_f << ErrorDescription;
				}
			}

			for (auto &StoreResult : PrivateStoreResults)
			{
				auto &WeakSecretsManager = PrivateStoreResults.fs_GetKey(StoreResult);

				if (!StoreResult)
				{
					SecretManagers.f_Remove(WeakSecretsManager);

					CStr ErrorDescription = "Failed to add signing certificate private secret to secrets manager '{}': {}"_f
						<< SecretManagerDescriptions[WeakSecretsManager]
						<< StoreResult.f_GetExceptionStr()
					;

					DMibLog(Error, "{}", ErrorDescription);
					*_pCommandLine %= "{}\n"_f << ErrorDescription;
				}
			}

			if (SecretManagers.f_IsEmpty())
			{
				*_pCommandLine %= "None of the secret managers were successful, signing certificate was not added.\n";

				co_return 1;
			}

			Auditor.f_Info("Added Signing Certificate '{}'"_f << SigningCertKey);
		}

		co_return 0;
	}
}
