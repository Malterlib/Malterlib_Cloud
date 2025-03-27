// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_CodeSigningManager.h"

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/CommandLine/TableRenderer>

namespace NMib::NCloud::NCodeSigningManager
{
	TCFuture<uint32> CCodeSigningManagerActor::fp_CommandLine_AuthorityResync(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto Auditor = f_Auditor();

		CStr AuthorityName = _Params["Authority"].f_String();

		auto AllSecretManagers = fg_VectorFromContainer(mp_SecretsManagerSubscription.m_Actors);

		TCVector<CStr> Authorities;
		for (auto &Authority : mp_Authorities)
		{
			auto &Name = Authority.f_GetName();
			if (!AuthorityName.f_IsEmpty() && Name != AuthorityName)
				continue;

			Authorities.f_Insert(Name);
		}

		for (auto &Name : Authorities)
		{
			CAuthority *pAuthority = nullptr;

			auto OnResume = co_await fg_OnResume
				(
					[&]() -> NException::CExceptionPointer
					{
						if (mp_State.m_bStoppingApp || f_IsDestroyed())
							return DMibErrorInstance("Startup aborted");

						pAuthority = mp_Authorities.f_FindEqual(Name);

						if (!pAuthority)
							return DMibErrorInstance("Certificate authority '{}' deleted"_f << Name);

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

			CCertificateAndKey AuthorityCertificate;
			AuthorityCertificate.m_Certificate = pAuthority->m_Certificate;
			try
			{
				AuthorityCertificate.m_Key = co_await fp_Authority_FetchPrivateKey(Name);
			}
			catch (CException const &_Exception)
			{
				CStr ErrorDescription = "Failed to fetch private key for authority '{}': {}"_f << Name << _Exception;
				DMibLog(Error, "{}", ErrorDescription);
				*_pCommandLine %= "{}\n"_f << ErrorDescription;
				continue;
			}

			TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> PublicStoreResultsAsync;
			TCFutureMap<TCWeakDistributedActor<CSecretsManager>, CSecretsManager::CSetSecretPropertiesResult> PrivateStoreResultsAsync;

			fp_Authority_StoreSecrets
				(
					AllSecretManagers
					, Name
					, pAuthority->m_Serial
					, pAuthority->m_PublicKeySetting
					, pAuthority->m_Created
					, pAuthority->m_LastModified
					, AuthorityCertificate
					, PublicStoreResultsAsync
					, PrivateStoreResultsAsync
				 )
			;

			{
				bool bUpdated = false;
				{
					auto PublicResults = co_await fg_AllDoneWrapped(PublicStoreResultsAsync);
					auto PrivateResults = co_await fg_AllDoneWrapped(PrivateStoreResultsAsync);

					for (auto &SecretManager : AllSecretManagers)
					{
						auto WeakSecretsManager = SecretManager.m_Actor.f_Weak();
						auto *pPublicResult = PublicResults.f_FindEqual(WeakSecretsManager);
						auto *pPrivateResult = PrivateResults.f_FindEqual(WeakSecretsManager);

						if (!pPublicResult || !*pPublicResult)
						{
							CStr ErrorDescription = "Failed to sync certificate authority public secret to secrets manager '{}': {}"_f
								<< SecretManagerDescriptions[WeakSecretsManager]
								<< (pPublicResult ? pPublicResult->f_GetExceptionStr() : CStr("missing result"))
							;

							DMibLog(Error, "{}", ErrorDescription);
							*_pCommandLine %= "{}\n"_f << ErrorDescription;
							continue;
						}

						if (!pPrivateResult || !*pPrivateResult)
						{
							CStr ErrorDescription = "Failed to sync certificate authority private secret to secrets manager '{}': {}"_f
								<< SecretManagerDescriptions[WeakSecretsManager]
								<< (pPrivateResult ? pPrivateResult->f_GetExceptionStr() : CStr("missing result"))
							;

							DMibLog(Error, "{}", ErrorDescription);
							*_pCommandLine %= "{}\n"_f << ErrorDescription;
							continue;
						}

						pAuthority->m_SecretsManagers[WeakSecretsManager] = pAuthority->m_LastModified;
						bUpdated = true;

						if ((*pPublicResult)->m_Flags & CSecretsManager::ESetSecretPropertiesResultFlag_Created)
						{
							Auditor.f_Info("Resync CodeSign authority: Created {}"_f << Name);
							*_pCommandLine %= "Created {} on {}\n"_f << Name << SecretManagerDescriptions[WeakSecretsManager];
						}
						else if ((*pPublicResult)->m_Flags & CSecretsManager::ESetSecretPropertiesResultFlag_Updated)
						{
							Auditor.f_Info("Resync CodeSign authority: Updated {}"_f << Name);
							*_pCommandLine %= "Updated {} on {}\n"_f << Name << SecretManagerDescriptions[WeakSecretsManager];
						}
					}
				}

				if (bUpdated)
					fp_Authority_UpdateStatus(*pAuthority, CDistributedAppSensorReporter::EStatusSeverity_Ok, "OK");
			}
		}

		co_return 0;
	}
}
