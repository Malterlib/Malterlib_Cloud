// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_CodeSigningManager.h"

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NCodeSigningManager
{
	CSecretsManager::CSecretID CCodeSigningManagerActor::CSigningCert::f_GetPublicSecretID() const
	{
		CSecretsManager::CSecretID SecretID;
		SecretID.m_Folder = mc_pSigningCertPublicFolder;
		SecretID.m_Name = "{}#{}"_f << f_GetKey().m_Authority << f_GetKey().m_Name;

		return SecretID;
	}

	CSecretsManager::CSecretID CCodeSigningManagerActor::CSigningCert::f_GetPrivateSecretID() const
	{
		CSecretsManager::CSecretID SecretID;
		SecretID.m_Folder = mc_pSigningCertPrivateFolder;
		SecretID.m_Name = "{}#{}"_f << f_GetKey().m_Authority << f_GetKey().m_Name;

		return SecretID;
	}

	bool CCodeSigningManagerActor::CSigningCert::fs_IsValidName(CStr const &_Name)
	{
		return fg_IsValidHostname(_Name);
	}

	TCFuture<CSecureByteVector> CCodeSigningManagerActor::fp_SigningCert_FetchPrivateKey(CSigningCertKey _SigningCertKey)
	{
		CSigningCertKey SigningCertKey = fg_Move(_SigningCertKey);
		CSigningCert *pSigningCert = nullptr;

		auto OnResume = co_await fg_OnResume
			(
				[&, SigningCertKey]() -> NException::CExceptionPointer
				{
					if (mp_State.m_bStoppingApp || f_IsDestroyed())
						return DMibErrorInstance("Startup aborted");

					pSigningCert = mp_SigningCerts.f_FindEqual(SigningCertKey);
					if (!pSigningCert)
						return DMibErrorInstance("No such signing certificate: '{}'"_f << SigningCertKey);

					return {};
				}
			)
		;

		CSecretsManager::CSecretID PrivateSecretID = pSigningCert->f_GetPrivateSecretID();

		TCVector<TCWeakDistributedActor<CSecretsManager>> SecretManagers;
		for (auto &Entry : pSigningCert->m_SecretsManagers)
			SecretManagers.f_Insert(pSigningCert->m_SecretsManagers.fs_GetKey(Entry));

		if (SecretManagers.f_IsEmpty())
			co_return DMibErrorInstance("No secret managers available for signing certificate '{}'"_f << SigningCertKey);

		for (auto &WeakSecretManager : SecretManagers)
		{
			auto SecretManager = WeakSecretManager.f_Lock();
			if (!SecretManager)
				continue;

			auto SecretResult = co_await SecretManager.f_CallActor(&CSecretsManager::f_GetSecret)(PrivateSecretID).f_Wrap();

			if (!SecretResult)
			{
				DMibLog(Warning, "Failed to fetch private key for signing certificate '{}' from secrets manager: {}", SigningCertKey, SecretResult.f_GetExceptionStr());
				continue;
			}

			auto &Secret = *SecretResult;

			if (Secret.f_GetTypeID() != CSecretsManager::ESecretType_StringMap)
			{
				DMibLog(Warning, "Signing certificate '{}' private secret has unexpected secret type", SigningCertKey);
				continue;
			}

			auto &SecretValues = Secret.f_Get<CSecretsManager::ESecretType_StringMap>();
			auto *pPrivateKey = SecretValues.f_FindEqual("PrivateKey");
			if (!pPrivateKey)
			{
				DMibLog(Warning, "Signing certificate '{}' private secret is missing 'PrivateKey'", SigningCertKey);
				continue;
			}

			co_return CSecureByteVector::fs_FromString(*pPrivateKey);
		}

		co_return DMibErrorInstance("Failed to fetch private key for signing certificate '{}' from all secret managers"_f << SigningCertKey);
	}

	TCFuture<void> CCodeSigningManagerActor::fp_SigningCert_Add(TCDistributedActor<CSecretsManager> _SecretsManager, CSecretsManager::CSecretID _SecretID)
	{
		auto Properties = co_await _SecretsManager.f_CallActor(&CSecretsManager::f_GetSecretProperties)(_SecretID);

		if (!Properties.m_SemanticID)
		{
			DMibLog(Warning, "Invalid semantic ID for secret '{}'", _SecretID);
			co_return {};
		}

		if (!Properties.m_SemanticID->f_StartsWith(mc_pSigningCertSemanticPrefix))
		{
			DMibLog(Warning, "Semantic ID doesn't start with expected prefix for secret '{}'", _SecretID);
			co_return {};
		}

		if (!Properties.m_Metadata)
		{
			DMibLog(Warning, "Missing meta data for secret '{}'", _SecretID);
			co_return {};
		}

		auto &MetaData = *Properties.m_Metadata;

		auto AuthorityAndNameStr = Properties.m_SemanticID->f_RemovePrefix(mc_pSigningCertSemanticPrefix);
		auto AuthorityAndName = AuthorityAndNameStr.f_Split("#");

		if (AuthorityAndName.f_GetLen() != 2)
		{
			DMibLog(Warning, "Invalid signing cert semantic ID for secret '{}': {}", _SecretID, AuthorityAndNameStr);
			co_return {};
		}

		CSigningCertKey SigningCertKey;
		SigningCertKey.m_Authority = AuthorityAndName[0];
		SigningCertKey.m_Name = AuthorityAndName[1];

		if (!CAuthority::fs_IsValidName(SigningCertKey.m_Authority))
		{
			DMibLog(Warning, "Invalid authority semantic ID for secret '{}'", _SecretID);
			co_return {};
		}

		if (!CSigningCert::fs_IsValidName(SigningCertKey.m_Name))
		{
			DMibLog(Warning, "Invalid signing cert semantic ID for secret '{}'", _SecretID);
			co_return {};
		}

		if (AuthorityAndNameStr != _SecretID.m_Name)
		{
			DMibLog(Warning, "Signing cert/Authority name doesn't match semantic ID for secret '{}': {}", _SecretID, AuthorityAndNameStr);
			co_return {};
		}

		if (!Properties.m_Secret)
		{
			DMibLog(Warning, "Missing secret value for secret '{}'", _SecretID);
			co_return {};
		}

		if (Properties.m_Secret->f_GetTypeID() != CSecretsManager::ESecretType_StringMap)
		{
			DMibLog(Warning, "Secret value is of wrong type (expected string map) for secret '{}'", _SecretID);
			co_return {};
		}

		auto &Secrets = Properties.m_Secret->f_Get<CSecretsManager::ESecretType_StringMap>();

		auto *pCertificate = Secrets.f_FindEqual("Certificate");
		if (!pCertificate)
		{
			DMibLog(Warning, "Secret value is missing 'Certificate' for secret '{}'", _SecretID);
			co_return {};
		}

		CPublicKeySetting PublicKeySetting = CPublicKeySettings_EC_secp521r1{};
		if (auto *pKeyType = MetaData.f_FindEqual("KeyType"))
		{
			if (!pKeyType->f_IsString())
			{
				DMibLog(Warning, "Invalid json type for KeyType in meta data for secret '{}'", _SecretID);
				co_return {};
			}

			try
			{
				PublicKeySetting = fsp_PublicKeySettingFromStr(pKeyType->f_String());
			}
			catch (CException const &)
			{
				DMibLog(Warning, "Invalid KeyType ({}) in meta data for secret  '{}'", pKeyType->f_String(), _SecretID);
				co_return {};
			}
		}
		else
		{
			DMibLog(Warning, "Missing KeyType in meta data for secret '{}'", _SecretID);
			co_return {};
		}

		if (!Properties.m_Modified)
		{
			DMibLog(Warning, "Missing modified time for secret '{}'", _SecretID);
			co_return {};
		}

		auto &ModifiedTime = *Properties.m_Modified;

		if (!Properties.m_Created)
		{
			DMibLog(Warning, "Missing created time for secret '{}'", _SecretID);
			co_return {};
		}

		auto &CreatedTime = *Properties.m_Created;

		auto &SigningCert = mp_SigningCerts[SigningCertKey];
		if (!SigningCert.m_LastModified.f_IsValid() || ModifiedTime > SigningCert.m_LastModified)
		{
			SigningCert.m_Certificate = CByteVector::fs_FromString(*pCertificate);
			SigningCert.m_PublicKeySetting = PublicKeySetting;
			SigningCert.m_LastModified = ModifiedTime;
			SigningCert.m_Created = CreatedTime;
		}

		SigningCert.m_SecretsManagers[_SecretsManager.f_Weak()] = ModifiedTime;
		fp_SigningCert_UpdateStatus(SigningCert, CDistributedAppSensorReporter::EStatusSeverity_Ok, "OK");
		fp_SigningCert_UpdateSensor(SigningCert.f_GetKey()) > fg_LogError("Update sensors", "Falied to update signing cert sensors");

		co_return {};
	}

	TCFuture<void> CCodeSigningManagerActor::fp_SigningCert_SecretsManagerAdded(TCDistributedActor<CSecretsManager> _SecretsManager, CTrustedActorInfo _Info)
	{
		CSecretsManager::CSubscribeToChanges SubscribeOptions;
		SubscribeOptions.m_SemanticID = CStr(mc_pSigningCertSemanticPrefix) + "*";
		SubscribeOptions.m_TagsExclusive = {"Public"};
		SubscribeOptions.m_fOnChanges = g_ActorFunctor / [_SecretsManager, this](CSecretsManager::CSecretChanges _Changes) -> TCFuture<void>
			{
				TCFutureVector<void> AddSecretResults;
				for (auto &Changed : _Changes.m_Changed)
				{
					auto &SecretID = _Changes.m_Changed.fs_GetKey(Changed);

					fp_SigningCert_Add(_SecretsManager, SecretID) > AddSecretResults;
				}

				for (auto &Result : co_await fg_AllDoneWrapped(AddSecretResults))
				{
					if (!Result)
						DMibLog(Error, "Failed to add signing cert '{}'", Result.f_GetExceptionStr());
				}

				for (auto &RemovedID : _Changes.m_Removed)
				{
					if (RemovedID.m_Folder != mc_pSigningCertPublicFolder)
						continue;

					auto AuthorityAndNameStr = RemovedID.m_Name;
					auto AuthorityAndName = AuthorityAndNameStr.f_Split("#");

					if (AuthorityAndName.f_GetLen() != 2)
						continue;

					CSigningCertKey SigningCertKey;
					SigningCertKey.m_Authority = AuthorityAndName[0];
					SigningCertKey.m_Name = AuthorityAndName[1];
					if (auto *pSigningCert = mp_SigningCerts.f_FindEqual(SigningCertKey))
					{
						if (pSigningCert->m_SecretsManagers.f_FindEqual(_SecretsManager))
						{
							pSigningCert->m_SecretsManagers.f_Remove(_SecretsManager);
							if (pSigningCert->m_SecretsManagers.f_IsEmpty())
								mp_SigningCerts.f_Remove(SigningCertKey);
						}
					}
				}

				co_return {};
			}
		;

		mp_SigningCertSubscriptions[_SecretsManager.f_Weak()] = co_await _SecretsManager.f_CallActor(&CSecretsManager::f_SubscribeToChanges)(fg_Move(SubscribeOptions));

		co_return {};
	}
}
