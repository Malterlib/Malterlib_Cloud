// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_CodeSigningManager.h"

#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NCodeSigningManager
{
	CStr const &CCodeSigningManagerActor::CAuthority::f_GetName() const
	{
		return TCMap<CStr, CAuthority>::fs_GetKey(*this);
	}

	CSecretsManager::CSecretID CCodeSigningManagerActor::CAuthority::f_GetPublicSecretID() const
	{
		CSecretsManager::CSecretID SecretID;
		SecretID.m_Folder = mc_pAuthorityPublicFolder;
		SecretID.m_Name = f_GetName();

		return SecretID;
	}

	CSecretsManager::CSecretID CCodeSigningManagerActor::CAuthority::f_GetPrivateSecretID() const
	{
		CSecretsManager::CSecretID SecretID;
		SecretID.m_Folder = mc_pAuthorityPrivateFolder;
		SecretID.m_Name = f_GetName();

		return SecretID;
	}

	TCFuture<int32> CCodeSigningManagerActor::fp_Authority_GetNewSerial(CStr _AuthorityName)
	{
		CAuthority *pAuthority = nullptr;

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> NException::CExceptionPointer
				{
					if (mp_State.m_bStoppingApp || f_IsDestroyed())
						return DMibErrorInstance("Startup aborted");

					pAuthority = mp_Authorities.f_FindEqual(_AuthorityName);
					if (!pAuthority)
						return DMibErrorInstance("No such authority: '{}'"_f << _AuthorityName);

					return {};
				}
			)
		;

		// This algorithm might waste serials that never get used, but should allow it to resolve eventually

		NTime::CClock TimeTaken(true);
		while (TimeTaken.f_GetTime() < 30.0)
		{
			CTime ModifiedTime = CTime::fs_NowUTC();
			int32 OldSerial = pAuthority->m_Serial;
			int32 NewSerial = OldSerial + 1;

			TCFutureVector<void> AsyncResults;

			if (pAuthority->m_SecretsManagers.f_IsEmpty())
				co_return DMibErrorInstance("No secret managers available to store new serial on");

			for (auto &UpdateTime : pAuthority->m_SecretsManagers)
			{
				auto &WeakSecretManager = pAuthority->m_SecretsManagers.fs_GetKey(UpdateTime);
				auto SecretManager = WeakSecretManager.f_Lock();
				if (!SecretManager)
					continue;

				CSecretsManager::CSetMetadata SetMetaData;

				SetMetaData.m_ID = pAuthority->f_GetPublicSecretID();
				SetMetaData.m_Key = "Serial";
				SetMetaData.m_Value = NewSerial;
				SetMetaData.m_ExpectedValue = OldSerial;
				SetMetaData.m_ModifiedTime = ModifiedTime;

				SecretManager.f_CallActor(&CSecretsManager::f_SetMetadata)(fg_Move(SetMetaData)) > AsyncResults;
			}

			{
				auto Results = co_await fg_AllDoneWrapped(AsyncResults);
				bool bAllSuccessful = true;
				for (auto &Result : Results)
				{
					if (!Result)
					{
						if (!Result.f_HasExceptionType<CExceptionSecretsManagerUnexpectedValue>())
							co_await (fg_Move(Results) | g_Unwrap); // Rethrow all errors

						bAllSuccessful = false;
						break;
					}
				}

				if (bAllSuccessful)
					co_return NewSerial;
			}

			co_await fg_Timeout(fp64(1.0) + fg_GetRandomFloat() * fp64(3.0)); // Wait to allow sync of new values and random to allow other managers to get a new serial successfully
		}

		co_return DMibErrorInstance("Timed out trying to get a new serial");
	}

	TCFuture<CSecureByteVector> CCodeSigningManagerActor::fp_Authority_FetchPrivateKey(CStr _AuthorityName)
	{
		CStr AuthorityName = fg_Move(_AuthorityName);
		CAuthority *pAuthority = nullptr;

		auto OnResume = co_await fg_OnResume
			(
				[&, AuthorityName]() -> NException::CExceptionPointer
				{
					if (mp_State.m_bStoppingApp || f_IsDestroyed())
						return DMibErrorInstance("Startup aborted");

					pAuthority = mp_Authorities.f_FindEqual(AuthorityName);
					if (!pAuthority)
						return DMibErrorInstance("No such authority: '{}'"_f << AuthorityName);

					return {};
				}
			)
		;

		CSecretsManager::CSecretID PrivateSecretID = pAuthority->f_GetPrivateSecretID();

		TCVector<TCWeakDistributedActor<CSecretsManager>> SecretManagers;
		for (auto &Entry : pAuthority->m_SecretsManagers)
			SecretManagers.f_Insert(pAuthority->m_SecretsManagers.fs_GetKey(Entry));

		if (SecretManagers.f_IsEmpty())
			co_return DMibErrorInstance("No secret managers available for authority '{}'"_f << AuthorityName);

		for (auto &WeakSecretManager : SecretManagers)
		{
			auto SecretManager = WeakSecretManager.f_Lock();
			if (!SecretManager)
				continue;

			auto SecretResult = co_await SecretManager.f_CallActor(&CSecretsManager::f_GetSecret)(PrivateSecretID).f_Wrap();
			if (!SecretResult)
			{
				DMibLog(Warning, "Failed to fetch private key for authority '{}' from secrets manager: {}", AuthorityName, SecretResult.f_GetExceptionStr());
				continue;
			}

			auto &Secret = *SecretResult;
			if (Secret.f_GetTypeID() != CSecretsManager::ESecretType_StringMap)
			{
				DMibLog(Warning, "Authority '{}' private secret has unexpected secret type", AuthorityName);
				continue;
			}

			auto &SecretValues = Secret.f_Get<CSecretsManager::ESecretType_StringMap>();
			auto *pPrivateKey = SecretValues.f_FindEqual("PrivateKey");
			if (!pPrivateKey)
			{
				DMibLog(Warning, "Authority '{}' private secret is missing 'PrivateKey'", AuthorityName);
				continue;
			}

			co_return CSecureByteVector::fs_FromString(*pPrivateKey);
		}

		co_return DMibErrorInstance("Failed to fetch private key for authority '{}' from all secret managers"_f << AuthorityName);
	}

	TCFuture<void> CCodeSigningManagerActor::fp_Authority_Add(TCDistributedActor<CSecretsManager> _SecretsManager, CSecretsManager::CSecretID _SecretID)
	{
		auto Properties = co_await _SecretsManager.f_CallActor(&CSecretsManager::f_GetSecretProperties)(_SecretID);

		if (!Properties.m_SemanticID)
		{
			DMibLog(Warning, "Invalid semantic ID for secret '{}'", _SecretID);
			co_return {};
		}

		if (!Properties.m_SemanticID->f_StartsWith(mc_pAuthoritySemanticPrefix))
		{
			DMibLog(Warning, "Semantic ID doesn't start with expected prefix for secret '{}'", _SecretID);
			co_return {};
		}

		CStr AuthorityName = Properties.m_SemanticID->f_RemovePrefix(mc_pAuthoritySemanticPrefix);

		if (!CAuthority::fs_IsValidName(AuthorityName))
		{
			DMibLog(Warning, "Invalid authority semantic ID for secret '{}': {}", _SecretID, AuthorityName);
			co_return {};
		}

		if (AuthorityName != _SecretID.m_Name)
		{
			DMibLog(Warning, "Authority name doesn't match semantic ID for secret '{}': {}", _SecretID, AuthorityName);
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

		if (!Properties.m_Metadata)
		{
			DMibLog(Warning, "Missing meta data for secret '{}'", _SecretID);
			co_return {};
		}

		auto &MetaData = *Properties.m_Metadata;

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

		int32 Serial = 0;
		if (auto *pSerial = MetaData.f_FindEqual("Serial"))
		{
			if (!pSerial->f_IsInteger())
			{
				DMibLog(Warning, "Invalid json type for Serial in meta data for secret '{}'", _SecretID);
				co_return {};
			}

			Serial = pSerial->f_Integer();
		}
		else
		{
			DMibLog(Warning, "Missing Serial in meta data for secret '{}'", _SecretID);
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

		auto &Authority = mp_Authorities[AuthorityName];
		if (!Authority.m_LastModified.f_IsValid() || ModifiedTime > Authority.m_LastModified)
		{
			Authority.m_Certificate = CByteVector::fs_FromString(*pCertificate);
			Authority.m_PublicKeySetting = PublicKeySetting;
			Authority.m_Serial = Serial;
			Authority.m_LastModified = ModifiedTime;
			Authority.m_Created = CreatedTime;
		}

		Authority.m_SecretsManagers[_SecretsManager.f_Weak()] = ModifiedTime;
		fp_Authority_UpdateStatus(Authority, CDistributedAppSensorReporter::EStatusSeverity_Ok, "OK");
		fp_Authority_UpdateSensor(AuthorityName) > fg_LogError("Update sensors", "Falied to update authority sensors");

		co_return {};
	}

	TCFuture<void> CCodeSigningManagerActor::fp_Authority_SecretsManagerAdded(TCDistributedActor<CSecretsManager> _SecretsManager, CTrustedActorInfo _Info)
	{
		CSecretsManager::CSubscribeToChanges SubscribeOptions;
		SubscribeOptions.m_SemanticID = CStr(mc_pAuthoritySemanticPrefix) + "*";
		SubscribeOptions.m_TagsExclusive = {"Public"};
		SubscribeOptions.m_fOnChanges = g_ActorFunctor / [_SecretsManager, this](CSecretsManager::CSecretChanges _Changes) -> TCFuture<void>
			{
				TCFutureVector<void> AddSecretResults;
				for (auto &Changed : _Changes.m_Changed)
				{
					auto &SecretID = _Changes.m_Changed.fs_GetKey(Changed);

					fp_Authority_Add(_SecretsManager, SecretID) > AddSecretResults;
				}

				for (auto &Result : co_await fg_AllDoneWrapped(AddSecretResults))
				{
					if (!Result)
						DMibLog(Error, "Failed to add authority '{}'", Result.f_GetExceptionStr());
				}

				for (auto &RemovedID : _Changes.m_Removed)
				{
					if (RemovedID.m_Folder != mc_pAuthorityPublicFolder)
						continue;

					if (auto *pAuthority = mp_Authorities.f_FindEqual(RemovedID.m_Name))
					{
						if (pAuthority->m_SecretsManagers.f_FindEqual(_SecretsManager))
						{
							pAuthority->m_SecretsManagers.f_Remove(_SecretsManager);
							if (pAuthority->m_SecretsManagers.f_IsEmpty())
								mp_Authorities.f_Remove(RemovedID.m_Name);
						}
					}
				}

				co_return {};
			}
		;

		mp_AuthoritySubscriptions[_SecretsManager.f_Weak()] = co_await _SecretsManager.f_CallActor(&CSecretsManager::f_SubscribeToChanges)(fg_Move(SubscribeOptions));

		co_return {};
	}
}
