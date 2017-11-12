// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_SecretsManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		auto SecretsManagerHost = "SecretsManagerHost?"_=
			{
				"Names"_= {"--host"}
				, "Default"_= ""
				, "Description"_= "Limit query to only specified host ID"
			}
		;
		auto IDParameter = "Parameters"_=
			{
				"ID"_=
				{
					"Type"_= ""
					, "Description"_= "Specify secret ID\n"
					"The ID is specified as Folder/Name with folder and name adhering to RFC 1123 (hostname)"
				}
			}
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-enumerate-secrets"}
					, "Description"_= "Enumerate secrets in the database"
					, "Options"_=
					{
						SecretsManagerHost
						, "SemanticID?"_=
						{
							"Names"_= {"--semantic-id"}
							, "Default"_= ""
							, "Description"_= "Limit query to secrets having the specified semantic ID\n"
							"The semantic ID must adhere to RFC 1123 (hostname)"
						}
						, "Tags?"_=
						{
							"Names"_= {"--tags"}
							, "Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "Limit query to secrets having the specified tags\n"
							"The tags are specified in a JSON array '[\"Tag1\", \"Tag2\" ...]' and the tags must adhere to RFC 1123 (hostname)"
						}
					}
				}
				, [this](CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_EnumerateSecrets(_Params, _pCommandLine);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-get-secret-by-semantic-id"}
					, "Description"_= "Get secret by semantic id and tags"
					, "Options"_=
					{
						SecretsManagerHost
					}
					, "Parameters"_=
					{
						"SemanticID"_=
						{
							"Type"_= ""
							, "Description"_= "Get the secret with the specified semantic ID\n"
							"The semantic ID must adhere to RFC 1123 (hostname)"
						}
						, "Tags?"_=
						{
							"Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "Limit query to secrets having the specified tags.\n"
							"The tags are specified in a JSON array '[\"Tag1\", \"Tag2\" ...]' and the tags must adhere to RFC 1123 (hostname)"
						}
					}
				}
				, [this](CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_GetSecretBySemanticID(_Params, _pCommandLine);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-get-secret-properties"}
					, "Description"_= "Get secret properties"
					, "Options"_=
					{
						SecretsManagerHost
					}
					, IDParameter
				}
				, [this](CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_GetProperties(_Params, _pCommandLine);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-get-secret"}
					, "Description"_= "Get secret"
					, "Options"_=
					{
						SecretsManagerHost
					}
					, IDParameter
				}
				, [this](CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_GetSecret(_Params, _pCommandLine);
				}
			)
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-set-secret-properties"}
					, "Description"_= "Set properties for a secret"
					, "Options"_=
					{
						SecretsManagerHost
						, "Secret?"_=
						{
							"Names"_= {"--secret"}
							, "Type"_= COneOf{"string", "binary"}
							, "Description"_= "Set the secret.\n"
							"The secret can be a string secret or a binary secret.\n"
							"Specify 'string' to be prompted for a string secret.\n"
							"Specify 'binary' for a binary secret. The binary secret must be piped or redirected to stdin\n"
						}
						, "SecretFile?"_=
						{
							"Names"_= {"--secret-file"}
							, "Type"_= ""
							, "Description"_= "The secret file to set.\n"
						}
						, "Username?"_=
						{
							"Names"_= {"--username"}
							, "Type"_= ""
							, "Description"_= "The username to set.\n"
						}
						, "URL?"_=
						{
							"Names"_= {"--URL"}
							, "Type"_= ""
							, "Description"_= "The URL to set.\n"
						}
						, "Expires?"_=
						{
							"Names"_= {"--expires"}
							, "Type"_= CTime()
							, "Description"_= "The time when the secret expires.\n"
						}
						, "Notes?"_=
						{
							"Names"_= {"--notes"}
							, "Type"_= ""
							, "Description"_= "The notes to set.\n"
						}
						, "Metadata?"_=
						{
							"Names"_= {"--metadata"}
							, "Type"_= EJSONType_Object
							, "Description"_= "The metadata to set.\n"
							"The metadata is specified as a JSON object '{\"Key\" : \"Value\" ...}'"
						}
						, "Created?"_=
						{
							"Names"_= {"--created"}
							, "Type"_= CTime()
							, "Description"_= "The time when the secret was created. Set automatically if not specified.\n"
						}
						, "Modified?"_=
						{
							"Names"_= {"--modified"}
							, "Type"_= CTime()
							, "Description"_= "The time when the secret was modified. Set automatically if not specified.\n"
						}
						, "Tags?"_=
						{
							"Names"_= {"--tags"}
							, "Type"_= {""}
							, "Description"_= "The tags to set.\n"
							"The tags are specified in a JSON array '[\"Tag1\", \"Tag2\" ...]' and the tags must adhere to RFC 1123 (hostname)"
						}
						, "SemanticID?"_=
						{
							"Names"_= {"--semantic-id"}
							, "Type"_= ""
							, "Description"_= "The semantic ID to set.\n"
							"The semantic ID must adhere to RFC 1123 (hostname)"
						}
					}
					, IDParameter
				}
				, [this](CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_SetProperties(_Params, _pCommandLine);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-change-tags"}
					, "Description"_= "Add tags to a secret"
					, "Options"_=
					{
						SecretsManagerHost
						, "RemoveTags?"_=
						{
							"Names"_= {"--remove"}
							, "Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "Remove these tags.\n"
							"The tags are specified in a JSON array '[\"Tag1\", \"Tag2\" ...]' and the tags must adhere to RFC 1123 (hostname)"
						}
						, "AddTags?"_=
						{
							"Names"_= {"--add"}
							, "Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "Add these tags.\n"
							"The tags are specified in a JSON array '[\"Tag1\", \"Tag2\" ...]' and the tags must adhere to RFC 1123 (hostname)"
						}
					}
					, "Parameters"_=
					{
						"ID"_=
						{
							"Type"_= ""
							, "Description"_= "Specify secret ID"
							"The ID is specified as Folder/Name with folder and name adhering to RFC 1123 (hostname)"
						}
					}
				}
				, [this](CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_ChangeTags(_Params, _pCommandLine);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-set-metadata"}
					, "Description"_= "Add metadata to a secret"
					, "Options"_=
					{
						SecretsManagerHost
					}
					, "Parameters"_=
					{
						"ID"_=
						{
							"Type"_= ""
							, "Description"_= "Specify secret ID"
							"The ID is specified as Folder/Name with folder and name adhering to RFC 1123 (hostname)"
						}
						, "Metadata"_=
						{
							"Type"_= EJSONType_Object
							, "Description"_= "The metadata to set.\n"
							"The metadata is specified as a JSON object '{\"Key\" : \"Value\"}'"
						}
					}
				}
				, [this](CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_SetMetadata(_Params, _pCommandLine);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-remove-metadata"}
					, "Description"_= "Remove metadata from a secret"
					, "Options"_=
					{
						SecretsManagerHost
					}
					, "Parameters"_=
					{
						"ID"_=
						{
							"Type"_= ""
							, "Description"_= "Specify secret ID"
							"The ID is specified as Folder/Name with folder and name adhering to RFC 1123 (hostname)"
						}
						, "Key"_=
						{
							"Type"_= ""
							, "Description"_= "Key of the metadata to remove.\n"
						}
					}
				}
				, [this](CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_RemoveMetadata(_Params, _pCommandLine);
				}
			)
		;
	}

	bool CCloudClientAppActor::fp_SecretsManager_SplitID(CEJSON const &_Params, CSecretsManager::CSecretID &o_ID, CStr &o_Error)
	{
		if (auto const &ID = _Params["ID"].f_String())
		{
			aint SeparatorPosition = fg_StrFindChar(ID, '/');
			if (SeparatorPosition > 0)
			{
				o_ID = {ID.f_Left(SeparatorPosition), ID.f_Extract(SeparatorPosition + 1)};
				if (!CSecretsManager::fs_IsValidTag(o_ID.m_Folder))
				{
					o_Error = fg_Format("'{}' is not a valid Folder", o_ID.m_Folder);
					return true;
				}
				if (!CSecretsManager::fs_IsValidTag(o_ID.m_Name))
				{
					o_Error = fg_Format("'{}' is not a valid Name", o_ID.m_Name);
					return true;
				}
				return false;
			}
			else
			{
				o_Error = "Expected Secret ID on the form: Folder/Name";
				return true;
			}
		}

		o_Error = "Expected a non-empty secret id";
		return true;
	}

	TCContinuation<void> CCloudClientAppActor::fp_SecretsManager_SubscribeToServers()
	{
		if (!mp_SecretsManagers.f_IsEmpty())
			return fg_Explicit();
		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to secrets managers");

		TCContinuation<void> Continuation;

		mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_SubscribeTrustedActors<NCloud::CSecretsManager>
				, "com.malterlib/Cloud/SecretsManager"
				, fg_ThisActor(this)
			)
			> [this, Continuation](TCAsyncResult<TCTrustedActorSubscription<CSecretsManager>> &&_Subscription)
			{
				if (!_Subscription)
				{
					DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscribe to secrets managers: {}", _Subscription.f_GetExceptionStr());
					Continuation.f_SetException(_Subscription);
					return;
				}
				mp_SecretsManagers = fg_Move(*_Subscription);
				if (mp_SecretsManagers.m_Actors.f_IsEmpty())
				{
					Continuation.f_SetException(DMibErrorInstance("Not connected to any secrets managers, or they are not trusted for 'com.malterlib/Cloud/SecretsManager' namespace"));
					return;
				}
				Continuation.f_SetResult();
			}
		;
		return Continuation;
	}

	template<typename tf_CType>
	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_Enumerate
		(
		 	CEJSON const &_Params
		 	, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			, TCFunction<TCContinuation<tf_CType>
		 		(
				 	TCDistributedActor<CSecretsManager> const &_Actor
				 	, TCSharedPointer<TCOptional<CStrSecure>> _pSemanticID
				 	, TCSharedPointer<TCSet<CStrSecure>> _pTags
				)> &&_fGetResult
			, TCFunction<void (tf_CType *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine)> &&_fOnResult
		)
	{
		TCContinuation<uint32> Continuation;

		CStr Host = _Params["SecretsManagerHost"].f_String();

		TCSharedPointer<TCOptional<CStrSecure>> pSemanticID = fg_Construct();
		if (auto ID = _Params["SemanticID"].f_String())
		{
			if (!CSecretsManager::fs_IsValidTag(ID))
			{
				Continuation.f_SetException(DMibErrorInstance(fg_Format("'{}' is not a valid Semantic ID", ID)));
				return Continuation;
			}
			*pSemanticID = ID;
		}

		TCSharedPointer<TCSet<CStrSecure>> pTags = fg_Construct();
		for (auto &TagJSON : _Params["Tags"].f_Array())
		{
			CStr const &Tag = TagJSON.f_String();
			if (!CSecretsManager::fs_IsValidTag(Tag))
			{
				Continuation.f_SetException(DMibErrorInstance(fg_Format("'{}' is not a valid tag", Tag)));
				return Continuation;
			}
			(*pTags)[Tag];
		}

		fg_ThisActor(this)(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers")
			> Continuation / [=]
			{
				TCActorResultMap<CHostInfo, tf_CType> Secrets;

				for (auto &TrustedActor : mp_SecretsManagers.m_Actors)
				{
					if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
						continue;

					_fGetResult(TrustedActor.m_Actor, pSemanticID, pTags).f_Dispatch().f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
						> Secrets.f_AddResult(TrustedActor.m_TrustInfo.m_HostInfo)
					;
				}

				Secrets.f_GetResults() > Continuation / [=](TCMap<CHostInfo, TCAsyncResult<tf_CType>> &&_Results)
					{
						tf_CType *pFirstResult = nullptr;

						for (auto &Result : _Results)
						{
							if (!Result)
							{
								*_pCommandLine %= "Failed getting secrets for this host: {}\n"_f << Result.f_GetExceptionStr();
								continue;
							}

							if (!pFirstResult)
								pFirstResult = &*Result;
							else if (*pFirstResult != *Result)
								return Continuation.f_SetException(DMibErrorInstance("Data inconsistency between secrets managers. Please try again later."));
						}

						if (pFirstResult)
							_fOnResult(pFirstResult, _pCommandLine);
						else
							return Continuation.f_SetException(DMibErrorInstance("No secrets found on any connected secret manager"));

						Continuation.f_SetResult(0);
					}
				;
			}
		;
		return Continuation;
	}

	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_EnumerateSecrets(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		return fp_CommandLine_SecretsManager_Enumerate<TCSet<CSecretsManager::CSecretID>>
			(
				_Params
			 	, _pCommandLine
			 	, [](TCDistributedActor<CSecretsManager> const &_Actor, TCSharedPointer<TCOptional<CStrSecure>> _pSemanticID, TCSharedPointer<TCSet<CStrSecure>> _pTags)
				{
					return DMibCallActor(_Actor, CSecretsManager::f_EnumerateSecrets, *_pSemanticID, *_pTags);
				}
			 	, [](TCSet<CSecretsManager::CSecretID> *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					for (auto &ID : *pResult)
						*_pCommandLine += "{}\n"_f << ID;
				}
			)
		;

	}

	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_GetSecretBySemanticID(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		return fp_CommandLine_SecretsManager_Enumerate<CSecretsManager::CSecret>
			(
				_Params
			 	, _pCommandLine
			 	, [](TCDistributedActor<CSecretsManager> const &_Actor, TCSharedPointer<TCOptional<CStrSecure>> _pSemanticID, TCSharedPointer<TCSet<CStrSecure>> _pTags)
				{
					return DMibCallActor(_Actor, CSecretsManager::f_GetSecretBySemanticID, **_pSemanticID, *_pTags);
				}
			 	, [](CSecretsManager::CSecret *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					auto &Secret = *pResult;
					if (Secret.f_GetTypeID() == CSecretsManager::ESecretType_Buffer)
						*_pCommandLine += Secret.f_Get<CSecretsManager::ESecretType_Buffer>();
					else
						*_pCommandLine += "{}\n"_f << Secret;
				}
			)
		;

	}

	template<typename tf_CType>
	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_Get
		(
		 	CEJSON const &_Params
		 	, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			, TCFunction<TCContinuation<tf_CType> (TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID)> &&_fGetResult
			, TCFunction<void (tf_CType *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine)> &&_fOnResult
		)
	{
		TCContinuation<uint32> Continuation;

		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fp_SecretsManager_SplitID(_Params, ID, Error))
			return DMibErrorInstance(Error);

		fg_ThisActor(this)(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers")
			> Continuation / [=]
			{
				TCActorResultMap<CHostInfo, tf_CType> Secrets;

				for (auto &TrustedActor : mp_SecretsManagers.m_Actors)
				{
					if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
						continue;

					_fGetResult(TrustedActor.m_Actor, ID).f_Dispatch().f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
						> Secrets.f_AddResult(TrustedActor.m_TrustInfo.m_HostInfo)
					;
				}

				Secrets.f_GetResults() > Continuation / [=](TCMap<CHostInfo, TCAsyncResult<tf_CType>> &&_Results)
					{
						tf_CType *pFirstResult = nullptr;

						for (auto &Result : _Results)
						{
							if (!Result)
							{
								*_pCommandLine %= "Failed getting secrets for this host: {}\n"_f << Result.f_GetExceptionStr();
								continue;
							}

							if (!pFirstResult)
								pFirstResult = &*Result;
							else if (*pFirstResult != *Result)
								return Continuation.f_SetException(DMibErrorInstance("Data inconsistency between secrets managers. Please try again later."));
						}

						if (pFirstResult)
							_fOnResult(pFirstResult, _pCommandLine);
						else
							return Continuation.f_SetException(DMibErrorInstance("No secrets found on any connected secret manager"));

						Continuation.f_SetResult(0);
					}
				;
			}
		;
		return Continuation;
	}

	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_GetSecret(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		return fp_CommandLine_SecretsManager_Get<CSecretsManager::CSecret>
			(
				_Params
			 	, _pCommandLine
			 	, [](TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID)
				{
					return DMibCallActor(_Actor, CSecretsManager::f_GetSecret, fg_TempCopy(_ID));
				}
			 	, [](CSecretsManager::CSecret *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					auto &Secret = *pResult;
					if (Secret.f_GetTypeID() == CSecretsManager::ESecretType_Buffer)
						*_pCommandLine += Secret.f_Get<CSecretsManager::ESecretType_Buffer>();
					else
						*_pCommandLine += "{}\n"_f << Secret;
				}
			)
		;
	}

	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_GetProperties(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		return fp_CommandLine_SecretsManager_Get<CSecretsManager::CSecretProperties>
			(
				_Params
			 	, _pCommandLine
			 	, [](TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID)
				{
					return DMibCallActor(_Actor, CSecretsManager::f_GetSecretProperties, fg_TempCopy(_ID));
				}
			 	, [](CSecretsManager::CSecretProperties *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					auto &Result = *pResult;

					if ((*Result.m_Secret).f_GetTypeID() == CSecretsManager::ESecretType_Buffer)
						*_pCommandLine += "Secret (base64): {}\n"_f << *Result.m_Secret;
					else
						*_pCommandLine += "Secret:          {}\n"_f << *Result.m_Secret;
					if (*Result.m_UserName)
						*_pCommandLine += "Username:        {}\n"_f << *Result.m_UserName;
					if (*Result.m_URL)
						*_pCommandLine += "URL:             {}\n"_f << *Result.m_URL;
					if ((*Result.m_Expires).f_IsValid())
						*_pCommandLine += "Expires:         {}\n"_f << *Result.m_Expires;
					if (*Result.m_Notes)
						*_pCommandLine += "Notes:           {}\n"_f << *Result.m_Notes;
					if (!(*Result.m_Metadata).f_IsEmpty())
						*_pCommandLine += "Metadata:\n{}"_f << *Result.m_Metadata;
					if ((*Result.m_Created).f_IsValid())
						*_pCommandLine += "Created:         {}\n"_f << *Result.m_Created;
					if ((*Result.m_Modified).f_IsValid())
						*_pCommandLine += "Modified:        {}\n"_f << *Result.m_Modified;
					if (*Result.m_SemanticID)
						*_pCommandLine += "SemanticID:      {}\n"_f << *Result.m_SemanticID;
					if (!(*Result.m_Tags).f_IsEmpty())
						*_pCommandLine += "Tags:            {vs}\n"_f << *Result.m_Tags;
				}
			)
		;
	}

	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_SetProperties(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCContinuation<uint32> Continuation;

		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fp_SecretsManager_SplitID(_Params, ID, Error))
			return DMibErrorInstance(Error);

		CSecretsManager::CSecretProperties Properties{};

		if (auto pValue = _Params.f_GetMember("Username"))
			Properties.f_SetUserName(pValue->f_String());

		if (auto pValue = _Params.f_GetMember("URL"))
			Properties.f_SetURL(pValue->f_String());

		if (auto pValue = _Params.f_GetMember("Expires"))
			Properties.f_SetExpires(pValue->f_Date());

		if (auto pValue = _Params.f_GetMember("Notes"))
			Properties.f_SetNotes(pValue->f_String());

		if (auto pValue = _Params.f_GetMember("Metadata"))
		{
			auto Object = pValue->f_Object();
			for (auto iMetaData = Object.f_OrderedIterator(); iMetaData; ++iMetaData)
				Properties.f_SetMetadata(iMetaData->f_Name(), fg_TempCopy(iMetaData->f_Value()));
		}

		if (auto pValue = _Params.f_GetMember("Created"))
			Properties.f_SetCreated(pValue->f_Date());

		if (auto pValue = _Params.f_GetMember("Modified"))
			Properties.f_SetModified(pValue->f_Date());

		if (auto pValue = _Params.f_GetMember("SemanticID"))
		{
			auto ID = pValue->f_String();
			if (!CSecretsManager::fs_IsValidTag(ID))
			{
				Continuation.f_SetException(DMibErrorInstance(fg_Format("'{}' is not a valid SemanticID", ID)));
				return Continuation;
			}
			Properties.f_SetSemanticID(ID);
		}

		if (auto pValue = _Params.f_GetMember("Tags"))
		{
			TCSet<CStrSecure> Tags;
			for (auto &TagJSON : pValue->f_Array())
			{
				CStr const &Tag = TagJSON.f_String();
				if (!CSecretsManager::fs_IsValidTag(Tag))
				{
					Continuation.f_SetException(DMibErrorInstance(fg_Format("'{}' is not a valid tag", Tag)));
					return Continuation;
				}
				Tags[Tag];
			}
			Properties.f_SetTags(fg_Move(Tags));
		}

		if (auto pValue = _Params.f_GetMember("SecretFile"))
		{
			Continuation.f_SetException(DMibErrorInstance("File secrets not yet handled"));
			return Continuation;
		}

		TCContinuation<CSecretsManager::CSecret> SecretContinuation;
		bool SecretWasSet = false;

		if (auto pValue = _Params.f_GetMember("Secret"))
		{
			SecretWasSet = true;
			if (*pValue == "string")
			{
				CStdInReaderPromptParams PasswordPrompt;
				PasswordPrompt.m_bPassword = false;
				PasswordPrompt.m_Prompt = "Enter secret: ";

				_pCommandLine->f_ReadPrompt(PasswordPrompt) > SecretContinuation / [=](CStrSecure &&_SecretString)
					{
						SecretContinuation.f_SetResult(CSecretsManager::CSecret{_SecretString});
					}
				;
			}
			else if (*pValue == "binary")
			{
				_pCommandLine->f_ReadBinary() > SecretContinuation / [=](CSecureByteVector &&_Secret)
					{
						SecretContinuation.f_SetResult(CSecretsManager::CSecret{_Secret});
					}
				;
			}
			else
				DNeverGetHere;
		}
		else
			SecretContinuation.f_SetResult(CSecretsManager::CSecret{});

		SecretContinuation > Continuation / [=](CSecretsManager::CSecret && _Secret) mutable
			{
				if (SecretWasSet)
					Properties.f_SetSecret(fg_Move(_Secret));

				fp_SecretsManager_SubscribeToServers().f_Dispatch().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers")
					> Continuation / [=]() mutable
					{
						CStr Error;
						auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
						if (!pSecretsManager)
						{
							Continuation.f_SetException
								(
									DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error))
								)
							;
							return;
						}

						DMibCallActor
							(
								pSecretsManager->m_Actor
								, CSecretsManager::f_SetSecretProperties
								, fg_Move(ID)
								, fg_Move(Properties)
							)
							.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
							>  Continuation	/ [=]
							{
								Continuation.f_SetResult(0);
							}
						;
					}
				;
			}
		;
		return Continuation;
	}

	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_ChangeTags(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCContinuation<uint32> Continuation;

		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fp_SecretsManager_SplitID(_Params, ID, Error))
			return DMibErrorInstance(Error);

		auto fParseTags = [](CEJSON const &_Tags)
			{
				TCSet<CStrSecure> OutTags;
				for (auto &TagJSON : _Tags.f_Array())
				{
					CStr const &Tag = TagJSON.f_String();
					if (!CVersionManager::fs_IsValidTag(Tag))
						DMibError(fg_Format("'{}' is not a valid tag", Tag));
					OutTags[Tag];
				}
				return OutTags;
			}
		;

		TCSet<CStr> AddTags;
		TCSet<CStr> RemoveTags;

		try
		{
			AddTags = fParseTags(_Params["AddTags"]);
			RemoveTags = fParseTags(_Params["RemoveTags"]);
		}
		catch (CException const &_Error)
		{
			return _Error;
		}

		if (AddTags.f_IsEmpty() && RemoveTags.f_IsEmpty())
			return DMibErrorInstance("No changes specified. Specify tags to add and remove with --add and --remove");

		fg_ThisActor(this)(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers")
			> Continuation / [=]() mutable
			{
				CStr Error;
				auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
				if (!pSecretsManager)
				{
					Continuation.f_SetException
						(
							DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error))
						)
					;
					return;
				}

				DMibCallActor
					(
						pSecretsManager->m_Actor
						, CSecretsManager::f_ModifyTags
						, fg_Move(ID)
					 	, fg_Move(RemoveTags)
					 	, fg_Move(AddTags)
					)
					.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
					>  Continuation	/ [=]
					{
						Continuation.f_SetResult(0);
					}
				;
			}
		;
		return Continuation;
	}

	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_SetMetadata(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCContinuation<uint32> Continuation;

		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fp_SecretsManager_SplitID(_Params, ID, Error))
			return DMibErrorInstance(Error);

		CStrSecure MetadataKey;
		CEJSON MetadataValue;
		if (auto pValue = _Params.f_GetMember("Metadata"))
		{
			auto Object = pValue->f_Object();
			if (auto iMetaData = Object.f_OrderedIterator())
			{
				MetadataKey = iMetaData->f_Name();
				MetadataValue = iMetaData->f_Value();
				++iMetaData;
				if (iMetaData)
					return DMibErrorInstance("Multiple key values specified");
			}
			else
				return DMibErrorInstance("No key value specified");
		}

		fg_ThisActor(this)(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers")
			> Continuation / [=]() mutable
			{
				CStr Error;
				auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
				if (!pSecretsManager)
				{
					Continuation.f_SetException
						(
							DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error))
						)
					;
					return;
				}

				DMibCallActor
					(
						pSecretsManager->m_Actor
						, CSecretsManager::f_SetMetadata
						, fg_Move(ID)
					 	, MetadataKey
						, fg_Move(MetadataValue)
					)
					.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
					>  Continuation	/ [=]
					{
						Continuation.f_SetResult(0);
					}
				;
			}
		;
		return Continuation;
	}

	TCContinuation<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_RemoveMetadata(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCContinuation<uint32> Continuation;

		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fp_SecretsManager_SplitID(_Params, ID, Error))
			return DMibErrorInstance(Error);

		CStrSecure Key;
		if (auto pValue = _Params.f_GetMember("Key"))
			Key = pValue->f_String();

		fg_ThisActor(this)(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers")
			> Continuation / [=]() mutable
			{
				CStr Error;
				auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
				if (!pSecretsManager)
				{
					Continuation.f_SetException
						(
							DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error))
						)
					;
					return;
				}

				DMibCallActor(pSecretsManager->m_Actor, CSecretsManager::f_RemoveMetadata, fg_Move(ID), Key)
					.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
					>  Continuation	/ [=]
					{
						Continuation.f_SetResult(0);
					}
				;
			}
		;
		return Continuation;
	}
}
