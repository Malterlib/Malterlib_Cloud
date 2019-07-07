// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Cloud/SecretsManagerUpload>
#include <Mib/Cloud/SecretsManagerDownload>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/CommandLine/TableRenderer>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_SecretsManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		auto SecretsManagerHost = "SecretsManagerHost?"_=
			{
				"Names"_= {"--host"}
				, "Default"_= ""
				, "Description"_= "Limit query to only specified host ID."
			}
		;
		auto BinaryAsBase64 = "BinaryAsBase64"_=
			{
				"Names"_= {"--binary-as-base64"}
				, "Default"_= true
				, "Description"_= "Binary secrets will be read and written as base64 encoded strings."
			}
		;
		auto IDParameter = "Parameters"_=
			{
				"ID"_=
				{
					"Type"_= ""
					, "Description"_= "Specify secret ID.\n"
					"The ID is specified as Folder/Name with folder and name adhering to RFC 1123 (hostname)"
				}
			}
		;
		auto CurrentDirectory = "CurrentDirectory?"_=
			{
				"Names"_= _[_]
				, "Default"_= CFile::fs_GetCurrentDirectory()
				, "Hidden"_= true
				, "Description"_= "Internal hidden option to forward current directory."
			}
		;
		auto Quiet = "Quiet?"_=
			{
				"Names"_= {"--quiet"}
				, "Default"_= false
				, "Description"_= "Suppress non-error output."
			}
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-enumerate-secrets"}
					, "Description"_= "List the IDs of all secrets in the database."
					, "Options"_=
					{
						SecretsManagerHost
						, "SemanticID?"_=
						{
							"Names"_= {"--semantic-id"}
							, "Default"_= ""
							, "Description"_= "Limit query to secrets having the specified semantic ID.\n"
							"The semantic ID must adhere to RFC 1123 (hostname)"
						}
						, "Tags?"_=
						{
							"Names"_= {"--tags"}
							, "Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "Limit query to secrets having the specified tags.\n"
							"The tags are specified in a JSON array '[\"Tag1\", \"Tag2\" ...]' and the tags must adhere to RFC 1123 (hostname)"
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_SecretsManager_EnumerateSecrets, _Params, _pCommandLine);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-get-secret-by-semantic-id"}
					, "Description"_= "Get secret matching the semantic id and tags."
					, "Options"_=
					{
						SecretsManagerHost
						, "Tags?"_=
						{
							"Names"_= {"--tags"}
							, "Default"_= _[_]
							, "Type"_= {""}
							, "Description"_= "Limit query to secrets having the specified tags.\n"
							"The tags are specified in a JSON array '[\"Tag1\", \"Tag2\" ...]' and the tags must adhere to RFC 1123 (hostname)"
						}
						, "Expect?"_=
						{
							"Names"_= {"--expect"}
							, "Type"_= COneOf{"string", "binary", "file"}
							, "Default"_= "string"
							, "Description"_= "Unless the secret is of the expected variant report an error.\n"
						}
						, BinaryAsBase64
					}
					, "Parameters"_=
					{
						"SemanticID"_=
						{
							"Type"_= ""
							, "Description"_= "Get the secret with the specified semantic ID.\n"
							"The semantic ID must adhere to RFC 1123 (hostname)"
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_SecretsManager_GetSecretBySemanticID, _Params, _pCommandLine);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-get-secret-properties"}
					, "Description"_= "List all properties for the secret."
					, "Options"_=
					{
						SecretsManagerHost
						, BinaryAsBase64
						, CTableRenderHelper::fs_OutputTypeOption()
					}
					, IDParameter
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_SecretsManager_GetProperties, _Params, _pCommandLine);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-get-secret"}
					, "Description"_= "Get secret."
					, "Output"_= "The secret formatted acconding to --expect"
					, "Options"_=
					{
						SecretsManagerHost
						, "Expect?"_=
						{
							"Names"_= {"--expect"}
							, "Type"_= COneOf{"string", "binary", "file"}
							, "Default"_= "string"
							, "Description"_= "Unless the secret is of the expected variant report an error.\n"
						}
						, BinaryAsBase64
					}
					, IDParameter
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_SecretsManager_GetSecret, _Params, _pCommandLine);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-set-secret-properties"}
					, "Description"_= "Set properties for a secret.\n"
					"Add a new secret or change the properties of an existing secret.\n"
					"When creating a new secret, properties that are not set on the command line will be assigned default values. "
					"The default values will be empty except for the Created and Modified properties which will be set to the current time.\n"
					"When an existing secret is modified, properties not specified on the command line will retain their previous values, except the Modified property.\n"
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
							"Specify 'binary' for a binary secret. When --binary-as-base64 is enabled (which it is by default) you will be prompted for a base64 encoded binary secret. "
							"When --binary-as-base64 is disabled, the raw binary secret must be piped or redirected to stdin\n"
							"To set a file secret use --secrets-manager-upload-file."
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
						, BinaryAsBase64
					}
					, IDParameter
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_SecretsManager_SetProperties, _Params, _pCommandLine);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-change-tags"}
					, "Description"_= "Remove or add tags from/to a secret."
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
					, IDParameter
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_SecretsManager_ChangeTags, _Params, _pCommandLine);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-set-metadata"}
					, "Description"_= "Set or add metadata to a secret.\n"
					"Add a new key, value pair to the secrets metadata or replace the value if the key already exists."
					, "Options"_=
					{
						SecretsManagerHost
						, "Metadata"_=
						{
							"Names"_= {"--metadata"}
							, "Type"_= EJSONType_Object
							, "Description"_= "The metadata to set.\n"
							"The metadata is specified as a JSON object '{\"Key\" : \"Value\"}'"
						}
					}
					, IDParameter
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_SecretsManager_SetMetadata, _Params, _pCommandLine);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-remove-metadata"}
					, "Description"_= "Remove the metadata matching key from the secret."
					, "Options"_=
					{
						SecretsManagerHost
						, "Key"_=
						{
							"Names"_= {"--key"}
							, "Type"_= ""
							, "Description"_= "Key of the metadata to remove.\n"
						}
					}
					, IDParameter
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_SecretsManager_RemoveMetadata, _Params, _pCommandLine);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-remove-secret"}
					, "Description"_= "Remove the secret."
					, "Options"_=
					{
						SecretsManagerHost
					}
					, IDParameter
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_SecretsManager_RemoveSecret, _Params, _pCommandLine);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-upload-file"}
					, "Description"_= "Upload a file."
					, "Options"_=
					{
						SecretsManagerHost
						, "SecretFile"_=
						{
							"Names"_= {"--secret-file"}
							, "Type"_= ""
							, "Description"_= "The secret file to set.\n"
						}
						, Quiet
						, CurrentDirectory
					}
					, IDParameter
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_SecretsManager_Upload, _Params, _pCommandLine);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--secrets-manager-download-file"}
					, "Description"_= "Download a file."
					, "Options"_=
					{
						SecretsManagerHost
						, "OutputFile?"_=
						{
							"Names"_= {"--output-file"}
							, "Type"_= ""
							, "Description"_= "Filename for the downloaded file.\n"
						}
						, "AllowOverwrite?"_=
						{
							"Names"_= {"--allow-overwrite"}
							, "Default"_= false
							, "Description"_= "Allow overwirte of destination file.\n"
							"Only valid when output file is specified, otherwise file overwrite is never allowed.\n"
						}
						, Quiet
						, CurrentDirectory
					}
					, IDParameter
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return self(&CCloudClientAppActor::fp_CommandLine_SecretsManager_Download, _Params, _pCommandLine);
				}
				, CDistributedAppCommandLineSpecification::ECommandFlag_WaitForRemotes
			)
		;
	}

	bool CCloudClientAppActor::fsp_SecretsManager_GetID(CEJSON const &_Params, CSecretsManager::CSecretID &o_ID, CStr &o_Error)
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

	CStr CCloudClientAppActor::fsp_SecretsManager_CheckExpect(CSecretsManager::CSecret const &_Secret, NStr::CStr _Expect, bool _bBinaryAsBase64)
	{
		if (_Expect)
		{
			if (_Expect == "string" && _Secret.f_GetTypeID() != CSecretsManager::ESecretType_String && _Secret.f_GetTypeID() != CSecretsManager::ESecretType_Binary)
				return "Only string secrets and binary secrets can be emitted in string form";
			if (_Expect == "binary")
			{
				if (_Secret.f_GetTypeID() != CSecretsManager::ESecretType_Binary)
					return "Only binary secrets can be emitted in binary form";
				if (_bBinaryAsBase64)
					return "Binary secrets cannot be emitted in raw binary form when --binary-as-base64 is enabled. Use --no-binary-as-base64 to emit secrets in raw binary form.";
			}
			if (_Expect == "file" && _Secret.f_GetTypeID() != CSecretsManager::ESecretType_File)
				return "Only file secrets can be emitted in file form";
		}
		return {};
	}

	TCFuture<void> CCloudClientAppActor::fp_SecretsManager_SubscribeToServers()
	{
		if (!mp_SecretsManagers.f_IsEmpty())
			co_return {};

		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to secrets managers");

		auto Subscription = co_await mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_SubscribeTrustedActors<NCloud::CSecretsManager>
				, "com.malterlib/Cloud/SecretsManager"
				, fg_ThisActor(this)
			)
			.f_Wrap()
		;

		if (!Subscription)
		{
			DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscribe to secrets managers: {}", Subscription.f_GetExceptionStr());
			co_return Subscription.f_GetException();
		}

		mp_SecretsManagers = fg_Move(*Subscription);
		if (mp_SecretsManagers.m_Actors.f_IsEmpty())
			co_return DMibErrorInstance("Not connected to any secrets managers, or they are not trusted for 'com.malterlib/Cloud/SecretsManager' namespace");

		co_return {};
	}

	template<typename tf_CType>
	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_Enumerate
		(
		 	CEJSON const &_Params
		 	, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			, TCFunctionMovable
		 	<
		 		TCFuture<tf_CType>
	 			(
				 	TCDistributedActor<CSecretsManager> const &_Actor
				 	, TCOptional<CStrSecure> const &_SemanticID
				 	, TCSet<CStrSecure> const &_Tags
				)
		 	> &&_fGetResult
			, TCFunctionMovable<NStr::CStr (tf_CType *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine, CStr const &_Expect, bool _bBinaryAsBase64)> &&_fOnResult
		)
	{
		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();

		bool bBinaryAsBase64 = true;
		if (auto pValue = _Params.f_GetMember("BinaryAsBase64"))
			bBinaryAsBase64 = pValue->f_Boolean();

		CStr Host = _Params["SecretsManagerHost"].f_String();

		CStr Expect;
		if (auto pValue = _Params.f_GetMember("Expect"))
			Expect = pValue->f_String();

		TCOptional<CStrSecure> SemanticID;
		if (auto ID = _Params["SemanticID"].f_String())
		{
			if (!CSecretsManager::fs_IsValidTag(ID))
				co_return DMibErrorInstance(fg_Format("'{}' is not a valid Semantic ID", ID));
			SemanticID = ID;
		}

		TCSet<CStrSecure> Tags;
		for (auto &TagJSON : _Params["Tags"].f_Array())
		{
			CStr const &Tag = TagJSON.f_String();
			if (!CSecretsManager::fs_IsValidTag(Tag))
				co_return DMibErrorInstance(fg_Format("'{}' is not a valid tag", Tag));

			Tags[Tag];
		}

		co_await self(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		TCActorResultMap<CHostInfo, tf_CType> Secrets;

		for (auto &TrustedActor : mp_SecretsManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			_fGetResult(TrustedActor.m_Actor, SemanticID, Tags).f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
				> Secrets.f_AddResult(TrustedActor.m_TrustInfo.m_HostInfo)
			;
		}

		TCMap<CHostInfo, TCAsyncResult<tf_CType>> Results = co_await Secrets.f_GetResults();

		tf_CType *pFirstResult = nullptr;

		for (auto &Result : Results)
		{
			if (!Result)
			{
				auto &HostInfo = Results.fs_GetKey(Result);
				*_pCommandLine %= "{}Failed getting secrets for host{} '{}': {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					<< Result.f_GetExceptionStr()
				;
				continue;
			}

			if (!pFirstResult)
				pFirstResult = &*Result;
			else if (*pFirstResult != *Result)
				co_return DMibErrorInstance("Data inconsistency between secrets managers. Please try again later.");
		}

		if (pFirstResult)
		{
			if (auto ErrorStr = _fOnResult(pFirstResult, _pCommandLine, Expect, bBinaryAsBase64))
				co_return DMibErrorInstance(ErrorStr);
		}
		else
			co_return DMibErrorInstance("No secrets found on any connected secret manager");

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_EnumerateSecrets(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		return self
			(
			 	&CCloudClientAppActor::fp_CommandLine_SecretsManager_Enumerate<TCSet<CSecretsManager::CSecretID>>
				, _Params
			 	, _pCommandLine
			 	, [](TCDistributedActor<CSecretsManager> const &_Actor, TCOptional<CStrSecure> const &_SemanticID, TCSet<CStrSecure> const &_Tags)
			 	-> TCFuture<TCSet<CSecretsManager::CSecretID>>
				{
					return _Actor.f_CallActor(&CSecretsManager::f_EnumerateSecrets)(_SemanticID, _Tags);
				}
			 	, [](TCSet<CSecretsManager::CSecretID> *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine, CStr const &_Expect, bool _bBinaryAsBase64) -> CStr
				{
					for (auto &ID : *pResult)
						*_pCommandLine += "{}\n"_f << ID;

					return "";
				}
			)
		;

	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_GetSecretBySemanticID
		(
		 	CEJSON const &_Params
		 	, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine
		)
	{
		return self
			(
			 	&CCloudClientAppActor::fp_CommandLine_SecretsManager_Enumerate<CSecretsManager::CSecret>
				, _Params
			 	, _pCommandLine
			 	, [](TCDistributedActor<CSecretsManager> const &_Actor, TCOptional<CStrSecure> const &_SemanticID, TCSet<CStrSecure> const &_Tags) -> TCFuture<CSecretsManager::CSecret>
				{
					return _Actor.f_CallActor(&CSecretsManager::f_GetSecretBySemanticID)(*_SemanticID, _Tags);
				}
			 	, [](CSecretsManager::CSecret *_pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine, CStr const &_Expect, bool _bBinaryAsBase64) -> CStr
				{
					auto &Secret = *_pResult;

					if (auto Error = fsp_SecretsManager_CheckExpect(Secret, _Expect, _bBinaryAsBase64))
						return Error;

					if (Secret.f_GetTypeID() == CSecretsManager::ESecretType_Binary && !_bBinaryAsBase64)
						*_pCommandLine += Secret.f_Get<CSecretsManager::ESecretType_Binary>();
					else
						*_pCommandLine += "{}\n"_f << Secret;

					return CStr{};
				}
			)
		;

	}

	template<typename tf_CType>
	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_Get
		(
		 	CEJSON const &_Params
		 	, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			, TCFunctionMovable<TCFuture<tf_CType> (TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID)> &&_fGetResult
			, TCFunctionMovable<NStr::CStr (tf_CType *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine, CStr const &_Expect, bool _bBinaryAsBase64)> &&_fOnResult
		)
	{
		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();

		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;
		CStr Expect;

		bool bBinaryAsBase64 = true;
		if (auto pValue = _Params.f_GetMember("BinaryAsBase64"))
			bBinaryAsBase64 = pValue->f_Boolean();

		if (auto pValue = _Params.f_GetMember("Expect"))
			Expect = pValue->f_String();

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

		co_await self(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		TCActorResultMap<CHostInfo, tf_CType> Secrets;

		for (auto &TrustedActor : mp_SecretsManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			_fGetResult(TrustedActor.m_Actor, ID).f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
				> Secrets.f_AddResult(TrustedActor.m_TrustInfo.m_HostInfo)
			;
		}

		TCMap<CHostInfo, TCAsyncResult<tf_CType>> Results = co_await Secrets.f_GetResults();

		tf_CType *pFirstResult = nullptr;

		for (auto &Result : Results)
		{
			if (!Result)
			{
				auto &HostInfo = Results.fs_GetKey(Result);
				*_pCommandLine %= "{}Failed getting secrets for host{} '{}': {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					<< Result.f_GetExceptionStr()
				;
				continue;
			}

			if (!pFirstResult)
				pFirstResult = &*Result;
			else if (*pFirstResult != *Result)
				co_return DMibErrorInstance("Data inconsistency between secrets managers. Please try again later.");
		}

		if (pFirstResult)
		{
			if (auto ErrorStr = _fOnResult(pFirstResult, _pCommandLine, Expect, bBinaryAsBase64))
				co_return DMibErrorInstance(ErrorStr);
		}
		else
			co_return DMibErrorInstance("No secrets found on any connected secret manager");

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_GetSecret(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		return self
			(
			 	&CCloudClientAppActor::fp_CommandLine_SecretsManager_Get<CSecretsManager::CSecret>
				, _Params
			 	, _pCommandLine
			 	, [](TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID) -> TCFuture<CSecretsManager::CSecret>
				{
					return _Actor.f_CallActor(&CSecretsManager::f_GetSecret)(fg_TempCopy(_ID));
				}
			 	, [](CSecretsManager::CSecret *_pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine, CStr const &_Expect, bool _bBinaryAsBase64) -> CStr
				{
					auto &Secret = *_pResult;

					if (auto Error = fsp_SecretsManager_CheckExpect(Secret, _Expect, _bBinaryAsBase64))
						return Error;

					if (Secret.f_GetTypeID() == CSecretsManager::ESecretType_Binary && !_bBinaryAsBase64)
						*_pCommandLine += Secret.f_Get<CSecretsManager::ESecretType_Binary>();
					else
						*_pCommandLine += "{}\n"_f << Secret;

					return CStr{};
				}
			)
		;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_GetProperties(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		return fp_CommandLine_SecretsManager_Get<CSecretsManager::CSecretProperties>
			(
				_Params
			 	, _pCommandLine
			 	, [](TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID) -> TCFuture<CSecretsManager::CSecretProperties>
				{
					return _Actor.f_CallActor(&CSecretsManager::f_GetSecretProperties)(fg_TempCopy(_ID));
				}
			 	, [](CSecretsManager::CSecretProperties *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine, CStr const &_Expect, bool _bBinaryAsBase64) -> CStr
				{
					auto &Result = *pResult;

					if ((*Result.m_Secret).f_GetTypeID() == CSecretsManager::ESecretType_File)
						*_pCommandLine += "Secret (file):   {}\n"_f << *Result.m_Secret;
					else if ((*Result.m_Secret).f_GetTypeID() == CSecretsManager::ESecretType_Binary && !_bBinaryAsBase64)
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

					return "";
				}
			)
		;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_SetProperties(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["SecretsManagerHost"].f_String();
		bool bBinaryAsBase64 = _Params["BinaryAsBase64"].f_Boolean();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

		CSecretsManager::CSecretProperties Properties{};
		aint nSetProperties = 0;

		if (auto pValue = _Params.f_GetMember("Username"))
		{
			Properties.f_SetUserName(pValue->f_String());
			++nSetProperties;
		}

		if (auto pValue = _Params.f_GetMember("URL"))
		{
			Properties.f_SetURL(pValue->f_String());
			++nSetProperties;
		}

		if (auto pValue = _Params.f_GetMember("Expires"))
		{
			Properties.f_SetExpires(pValue->f_Date());
			++nSetProperties;
		}

		if (auto pValue = _Params.f_GetMember("Notes"))
		{
			Properties.f_SetNotes(pValue->f_String());
			++nSetProperties;
		}

		if (auto pValue = _Params.f_GetMember("Metadata"))
		{
			auto Object = pValue->f_Object();
			for (auto iMetaData = Object.f_OrderedIterator(); iMetaData; ++iMetaData)
				Properties.f_SetMetadata(iMetaData->f_Name(), fg_TempCopy(iMetaData->f_Value()));
			++nSetProperties;
		}

		if (auto pValue = _Params.f_GetMember("Created"))
		{
			Properties.f_SetCreated(pValue->f_Date());
			++nSetProperties;
		}

		if (auto pValue = _Params.f_GetMember("Modified"))
		{
			Properties.f_SetModified(pValue->f_Date());
			++nSetProperties;
		}

		if (auto pValue = _Params.f_GetMember("SemanticID"))
		{
			auto ID = pValue->f_String();

			if (!CSecretsManager::fs_IsValidTag(ID))
				co_return DMibErrorInstance(fg_Format("'{}' is not a valid SemanticID", ID));

			Properties.f_SetSemanticID(ID);
			++nSetProperties;
		}

		if (auto pValue = _Params.f_GetMember("Tags"))
		{
			TCSet<CStrSecure> Tags;
			for (auto &TagJSON : pValue->f_Array())
			{
				CStr const &Tag = TagJSON.f_String();

				if (!CSecretsManager::fs_IsValidTag(Tag))
					co_return DMibErrorInstance(fg_Format("'{}' is not a valid tag", Tag));

				Tags[Tag];
			}
			Properties.f_SetTags(fg_Move(Tags));
			++nSetProperties;
		}

		CSecretsManager::CSecret Secret;
		bool SecretWasSet = false;

		if (auto pValue = _Params.f_GetMember("Secret"))
		{
			++nSetProperties;
			SecretWasSet = true;
			CStdInReaderPromptParams PasswordPrompt;
			PasswordPrompt.m_bPassword = true;
			if (*pValue == "string")
			{
				PasswordPrompt.m_Prompt = "Enter secret: ";
				Secret = CSecretsManager::CSecret{co_await _pCommandLine->f_ReadPrompt(PasswordPrompt)};
			}
			else if (*pValue == "binary")
			{
				if (bBinaryAsBase64)
				{
					PasswordPrompt.m_Prompt = "Enter base64 encoded secret: ";
					CStrSecure Encoded = co_await _pCommandLine->f_ReadPrompt(PasswordPrompt);

					CSecureByteVector Decoded;
					try
					{
						CDisableExceptionTraceScope Disabled;
						fg_Base64Decode(Encoded, Decoded);
					}
					catch (CException const &_Exception)
					{
						co_return DMibErrorInstance(fg_Format("Base64 decoding failed: {}", _Exception));
					}

					Secret = CSecretsManager::CSecret{fg_Move(Decoded)};
				}
				else
					Secret = CSecretsManager::CSecret{co_await _pCommandLine->f_ReadBinary()};
			}
			else
				DNeverGetHere;
		}

		if (nSetProperties == 0)
			co_return DMibErrorInstance("No properties specified. Specify at least one property to change");

		if (SecretWasSet)
			Properties.f_SetSecret(fg_Move(Secret));

		co_await self(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		co_await pSecretsManager->m_Actor.f_CallActor(&CSecretsManager::f_SetSecretProperties)(fg_Move(ID), fg_Move(Properties))
			.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
		;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_ChangeTags(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

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

		TCSet<CStrSecure> AddTags;
		TCSet<CStrSecure> RemoveTags;

		try
		{
			AddTags = fParseTags(_Params["AddTags"]);
			RemoveTags = fParseTags(_Params["RemoveTags"]);
		}
		catch (CException const &_Error)
		{
			co_return _Error.f_ExceptionPointer();
		}

		if (AddTags.f_IsEmpty() && RemoveTags.f_IsEmpty())
			co_return DMibErrorInstance("No changes specified. Specify tags to add and remove with --add and --remove");

		co_await self(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		co_await pSecretsManager->m_Actor.f_CallActor(&CSecretsManager::f_ModifyTags)(fg_Move(ID), fg_Move(RemoveTags), fg_Move(AddTags))
			.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
		;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_SetMetadata(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

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
					co_return DMibErrorInstance("Multiple key values specified");
			}
			else
				co_return DMibErrorInstance("No key value specified");
		}

		co_await self(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		co_await pSecretsManager->m_Actor.f_CallActor(&CSecretsManager::f_SetMetadata)(fg_Move(ID), MetadataKey, fg_Move(MetadataValue))
			.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
		;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_RemoveMetadata(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

		CStrSecure Key;
		if (auto pValue = _Params.f_GetMember("Key"))
			Key = pValue->f_String();

		co_await self(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		co_await pSecretsManager->m_Actor.f_CallActor(&CSecretsManager::f_RemoveMetadata)(fg_Move(ID), Key)
			.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
		;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_RemoveSecret(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

		co_await self(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		co_await pSecretsManager->m_Actor.f_CallActor(&CSecretsManager::f_RemoveSecret)(fg_Move(ID))
			.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
		;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_Upload(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

		bool bQuiet = _Params["Quiet"].f_Boolean();
		CStr Filename = _Params["SecretFile"].f_String();

		if (!Filename)
			co_return DMibErrorInstance("Secret file name is empty");

		Filename = CFile::fs_GetExpandedPath(Filename, _Params["CurrentDirectory"].f_String());

		// If link, resolve it so we transfer the actual file content and a not a link to it
		mint nLinks = 0;
		while (CFile::fs_FileExists(Filename, NFile::EFileAttrib_Link))
		{
			auto NewFilename = NFile::CFile::fs_ResolveSymbolicLink(Filename);
			Filename = CFile::fs_GetExpandedPath(NewFilename, CFile::fs_GetPath(Filename));
			if (++nLinks > 16)
				co_return DMibErrorInstance("Encountered too many symbolic links");
		}

		if (!CFile::fs_FileExists(Filename, NFile::EFileAttrib_File))
			co_return DMibErrorInstance("The secret file must be a file");

		co_await self(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		CDirectorySyncSend::CSyncResult	Result = co_await
			(
				NCloud::fg_UploadSecretFile
				(
					pSecretsManager->m_Actor
					, mp_State.m_DistributionManager
					, fg_Move(ID)
					, CDirectorySyncSend::CConfig(Filename)
					, mp_UploadSubscription
				)
				% "Failed to transfer secret file"
			)
		;
		if (!bQuiet)
		{
			if (Result.m_Stats.m_nSyncedFiles <= 1)
				*_pCommandLine += "All files were already up to date\n";
			else
			{
				*_pCommandLine +=
					"Upload finished transferring: {ns } incoming bytes at {fe2} MB/s {ns } outgoing bytes at {fe2} MB/s\n"_f
					<< Result.m_Stats.m_IncomingBytes
					<< Result.m_Stats.f_IncomingBytesPerSecond()/1'000'000.0
					<< Result.m_Stats.m_OutgoingBytes
					<< Result.m_Stats.f_OutgoingBytesPerSecond()/1'000'000.0
				;
			}
		}

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_Download(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

		bool bQuiet = _Params["Quiet"].f_Boolean();
		bool bAllowOverwrite = _Params["AllowOverwrite"].f_Boolean();

		CStr OutFile;
		if (auto pValue = _Params.f_GetMember("OutputFile"))
			OutFile = pValue->f_String();

		CStr CurrentDirectory = _Params["CurrentDirectory"].f_String();

		co_await self(&CCloudClientAppActor::fp_SecretsManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		CStr Destination;
		CDirectorySyncReceive::EEasyConfigFlag Flags = CDirectorySyncReceive::EEasyConfigFlag_None;

		if (bAllowOverwrite)
			Flags |= CDirectorySyncReceive::EEasyConfigFlag_AllowOverwrite;

		if (OutFile)
			Destination = CFile::fs_GetExpandedPath(OutFile, CurrentDirectory);
		else
		{
			Destination = CurrentDirectory;
			Flags |= CDirectorySyncReceive::EEasyConfigFlag_DestinationIsDirectory;
		}

		NFile::CDirectorySyncReceive::CSyncResult Result = co_await
			(
				fg_DownloadSecretFile(pSecretsManager->m_Actor, fg_Move(ID), CDirectorySyncReceive::CConfig(Destination, Flags))
			 	% "Failed to transfer secret file"
			)
		;

		if (!bQuiet)
		{
			if (Result.m_Stats.m_nSyncedFiles <= 1)
				*_pCommandLine += "All files were already up to date\n";
			else
			{
				*_pCommandLine +=
					"Download finished transferring: {ns } incoming bytes at {fe2} MB/s {ns } outgoing bytes at {fe2} MB/s\n"_f
					<< Result.m_Stats.m_IncomingBytes
					<< Result.m_Stats.f_IncomingBytesPerSecond()/1'000'000.0
					<< Result.m_Stats.m_OutgoingBytes
					<< Result.m_Stats.f_OutgoingBytesPerSecond()/1'000'000.0
				;
			}
		}

		co_return 0;
	}
}
