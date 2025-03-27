// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_CodeSigningManager.h"

namespace NMib::NCloud::NCodeSigningManager
{
	TCFuture<void> CCodeSigningManagerActor::fp_SecretsManagerAdded
		(
			TCDistributedActor<CSecretsManager> _SecretsManager
			, CTrustedActorInfo _Info
		)
	{
		co_await fp_Authority_SecretsManagerAdded(_SecretsManager, _Info);
		co_await fp_SigningCert_SecretsManagerAdded(_SecretsManager, _Info);

		co_return {};
	}

	TCFuture<void> CCodeSigningManagerActor::fp_SecretsManagerAddedWithRetry(TCDistributedActor<CSecretsManager> _SecretsManager, CTrustedActorInfo _Info)
	{
		auto Result = co_await fp_SecretsManagerAdded(_SecretsManager, _Info).f_Wrap();

		if (Result)
		{
			mp_LastSecretsManagerError.f_Remove(_SecretsManager);

			co_return {};
		}

		auto &LastError = mp_LastSecretsManagerError[_SecretsManager];

		auto Error = Result.f_GetExceptionStr();

		if (Error != LastError)
		{
			DLogWithCategory(Mib/Cloud/CodeSigningManager, Error, "Failed to handle secrets manager added for '{}' (will retry every 10 seconds): {}", _Info.m_HostInfo, Error);
			LastError = Error;
		}

		if (!mp_SecretsManagerSubscription.m_Actors.f_FindEqual(_SecretsManager))
			co_return {};

		if (mp_RetryingSecretsManagers(_SecretsManager).f_WasCreated())
		{
			fg_Timeout(10.0) > [=, this]() -> TCFuture<void>
				{
					mp_RetryingSecretsManagers.f_Remove(_SecretsManager);

					if (!mp_SecretsManagerSubscription.m_Actors.f_FindEqual(_SecretsManager))
						co_return {};

					co_await fp_SecretsManagerAddedWithRetry(_SecretsManager, _Info).f_Wrap() > fg_LogError("SecretsManager", "Failed to handle secrets manager added");

					co_return {};
				}
			;
		}

		co_return {};
	}

	TCFuture<void> CCodeSigningManagerActor::fp_SecretsManagerRemoved(TCWeakDistributedActor<CActor> _SecretsManager, CTrustedActorInfo _ActorInfo)
	{
		for (auto &Authority : mp_Authorities)
		{
			Authority.m_SecretsManagers.f_Remove(_SecretsManager);
			if (Authority.m_SecretsManagers.f_IsEmpty())
				fp_Authority_UpdateStatus(Authority, CDistributedAppSensorReporter::EStatusSeverity_Warning, "No secret manager connected");
		}
		for (auto &SigningCert : mp_SigningCerts)
		{
			SigningCert.m_SecretsManagers.f_Remove(_SecretsManager);
			if (SigningCert.m_SecretsManagers.f_IsEmpty())
				fp_SigningCert_UpdateStatus(SigningCert, CDistributedAppSensorReporter::EStatusSeverity_Warning, "No secret manager connected");
		}

		mp_LastSecretsManagerError.f_Remove(_SecretsManager);
		mp_RetryingSecretsManagers.f_Remove(_SecretsManager);

		if (auto *pSubscription = mp_SigningCertSubscriptions.f_FindEqual(_SecretsManager))
		{
			(**pSubscription).f_Destroy().f_DiscardResult();
			mp_SigningCertSubscriptions.f_Remove(_SecretsManager);
		}
		
		if (auto *pSubscription = mp_AuthoritySubscriptions.f_FindEqual(_SecretsManager))
		{
			(**pSubscription).f_Destroy().f_DiscardResult();
			mp_AuthoritySubscriptions.f_Remove(_SecretsManager);
		}

		co_return {};
	}
}
