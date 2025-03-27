// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_CodeSigningManager.h"

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/CommandLine/TableRenderer>

namespace NMib::NCloud::NCodeSigningManager
{
	TCFuture<uint32> CCodeSigningManagerActor::fp_CommandLine_SigningCertResync(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto Auditor = f_Auditor();

		CStr SigningCertName = _Params["SigningCert"].f_String();
		CStr AuthorityName = _Params["Authority"].f_String();

		auto AllSecretManagers = fg_VectorFromContainer(mp_SecretsManagerSubscription.m_Actors);

		TCVector<CSigningCertKey> SigningCerts;
		for (auto &SigningCert : mp_SigningCerts)
		{
			auto &SigningCertKey = SigningCert.f_GetKey();
			if (!SigningCertName.f_IsEmpty() && SigningCertKey.m_Name != SigningCertName)
				continue;

			if (!AuthorityName.f_IsEmpty() && SigningCertKey.m_Authority != AuthorityName)
				continue;

			auto *pAuthority = mp_Authorities.f_FindEqual(SigningCertKey.m_Authority);

			if (!pAuthority)
			{
				*_pCommandLine %= "Authority '{}' for signing certificate '{}' does not exist"_f << AuthorityName << SigningCertKey.m_Name;
				continue;
			}

			SigningCerts.f_Insert(SigningCertKey);
		}

		for (auto &SigningCertKey : SigningCerts)
		{
			CSigningCert *pSigningCert = nullptr;
			CAuthority *pAuthority = nullptr;

			auto OnResume = co_await fg_OnResume
				(
					[&]() -> NException::CExceptionPointer
					{
						if (mp_State.m_bStoppingApp || f_IsDestroyed())
							return DMibErrorInstance("Startup aborted");

						pSigningCert = mp_SigningCerts.f_FindEqual(SigningCertKey);
						if (!pSigningCert)
							return DMibErrorInstance("Signing certificate '{}' deleted"_f << SigningCertKey);

						pAuthority = mp_Authorities.f_FindEqual(SigningCertKey.m_Authority);
						if (!pAuthority)
							return DMibErrorInstance("Authority '{}' deleted"_f << SigningCertKey.m_Authority);

						return {};
					}
				)
			;

			TCMap<TCWeakDistributedActor<CSecretsManager>, CStr> SecretManagerDescriptions;

			for (auto &SecretManager : AllSecretManagers)
			{
				auto WeakSecretsManager = SecretManager.m_Actor.f_Weak();
				SecretManagerDescriptions[WeakSecretsManager] = "{}"_f << SecretManager.m_TrustInfo.m_HostInfo;
			}

			CSigningCertKey SigningCertKeyCopy = SigningCertKey;
			CSecureByteVector PrivateKey = co_await fp_SigningCert_FetchPrivateKey(fg_Move(SigningCertKeyCopy));

			CCertificateAndKey Certificate;
			Certificate.m_Certificate = pSigningCert->m_Certificate;
			Certificate.m_Key = fg_Move(PrivateKey);

			TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> PublicStoreResultsAsync;
			TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> PrivateStoreResultsAsync;

			fp_SigningCert_StoreSecrets
				(
					AllSecretManagers
					, *pAuthority
					, SigningCertKey
					, pSigningCert->m_PublicKeySetting
					, pSigningCert->m_Created
					, pSigningCert->m_LastModified
					, Certificate
					, PublicStoreResultsAsync
					, PrivateStoreResultsAsync
				)
			;

			{
				bool bUpdated = false;
				{
					auto PublicStoreResults = co_await fg_AllDoneWrapped(PublicStoreResultsAsync);
					auto PrivateStoreResults = co_await fg_AllDoneWrapped(PrivateStoreResultsAsync);

					for (auto &StoreResult : PublicStoreResults)
					{
						auto &WeakSecretsManager = PublicStoreResults.fs_GetKey(StoreResult);

						if (!StoreResult)
						{
							CStr ErrorDescription = "Failed to sync signing certificate public secret to secrets manager '{}': {}"_f
								<< SecretManagerDescriptions[WeakSecretsManager]
								<< StoreResult.f_GetExceptionStr()
							;

							DMibLog(Error, "{}", ErrorDescription);
							*_pCommandLine %= "{}\n"_f << ErrorDescription;
							continue;
						}

						pSigningCert->m_SecretsManagers[WeakSecretsManager] = pSigningCert->m_LastModified;
						bUpdated = true;

						if (StoreResult->m_Flags & CSecretsManager::ESetSecretPropertiesResultFlag_Created)
						{
							Auditor.f_Info("Resync CodeSign certificate signing certificate: Created public {}"_f << SigningCertKey);
							*_pCommandLine %= "Created public {} on {}\n"_f << SigningCertKey << SecretManagerDescriptions[WeakSecretsManager];
						}
						else if (StoreResult->m_Flags & CSecretsManager::ESetSecretPropertiesResultFlag_Updated)
						{
							Auditor.f_Info("Resync CodeSign certificate signing certificate: Updated public {}"_f << SigningCertKey);
							*_pCommandLine %= "Updated public {} on {}\n"_f << SigningCertKey << SecretManagerDescriptions[WeakSecretsManager];
						}
					}

					for (auto &StoreResult : PrivateStoreResults)
					{
						auto &WeakSecretsManager = PrivateStoreResults.fs_GetKey(StoreResult);

						if (!StoreResult)
						{
							CStr ErrorDescription = "Failed to sync signing certificate private secret to secrets manager '{}': {}"_f
								<< SecretManagerDescriptions[WeakSecretsManager]
								<< StoreResult.f_GetExceptionStr()
							;

							DMibLog(Error, "{}", ErrorDescription);
							*_pCommandLine %= "{}\n"_f << ErrorDescription;
							continue;
						}

						if (StoreResult->m_Flags & CSecretsManager::ESetSecretPropertiesResultFlag_Created)
						{
							Auditor.f_Info("Resync CodeSign certificate signing certificate: Created private {}"_f << SigningCertKey);
							*_pCommandLine %= "Created private {} on {}\n"_f << SigningCertKey << SecretManagerDescriptions[WeakSecretsManager];
						}
						else if (StoreResult->m_Flags & CSecretsManager::ESetSecretPropertiesResultFlag_Updated)
						{
							Auditor.f_Info("Resync CodeSign certificate signing certificate: Updated private {}"_f << SigningCertKey);
							*_pCommandLine %= "Updated private {} on {}\n"_f << SigningCertKey << SecretManagerDescriptions[WeakSecretsManager];
						}
					}
				}

				if (bUpdated)
					fp_SigningCert_UpdateStatus(*pSigningCert, CDistributedAppSensorReporter::EStatusSeverity_Ok, "OK");
			}
		}

		co_return 0;
	}
}
