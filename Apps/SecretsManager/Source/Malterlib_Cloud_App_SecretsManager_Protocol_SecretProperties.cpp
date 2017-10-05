// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_EnumerateSecrets
		(
			TCOptional<CStrSecure const> &_SemanticID
			, TCSet<CStrSecure> const &_TagsExclusive
		) -> TCContinuation<TCSet<CSecretID>>
	{
		TCContinuation<TCSet<CSecretID>> Continuation;
		return Continuation;
	}

	TCContinuation<CSecretsManager::CSecret> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecret(CSecretsManager::CSecretID &&_ID)
	{
		TCContinuation<CSecretsManager::CSecret> Continuation;
		return Continuation;
	}

	TCContinuation<CSecretsManager::CSecretProperties> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecretProperties(CSecretsManager::CSecretID &&_ID)
	{
		TCContinuation<CSecretsManager::CSecretProperties> Continuation;
		return Continuation;
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecretBySemanticID(CStrSecure const &_SemanticID, TCSet<CStrSecure> const &_TagsExclusive)
		-> TCContinuation<CSecretsManager::CSecret>
	{
		TCContinuation<CSecretsManager::CSecret> Continuation;
		return Continuation;
	}
	
	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_SetSecretProperties(CSecretsManager::CSecretID &&_ID, CSecretsManager::CSecretProperties &&_Secret)
		-> TCContinuation<void>
	{
		TCContinuation<void> Continuation;
		return Continuation;
	}

	TCContinuation<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_ModifyTags
		(
			CSecretsManager::CSecretID &&_ID
			, TCSet<CStrSecure> &&_TagsToRemove
			, TCSet<CStrSecure> &&_TagsToAdd
		)
	{
		TCContinuation<void> Continuation;
		return Continuation;
	}
	
	TCContinuation<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_SetMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey, CEJSON &&_Metadata)
	{
		TCContinuation<void> Continuation;
		return Continuation;
	}
	
	TCContinuation<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_RemoveMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey)
	{
		TCContinuation<void> Continuation;
		return Continuation;
	}

}
