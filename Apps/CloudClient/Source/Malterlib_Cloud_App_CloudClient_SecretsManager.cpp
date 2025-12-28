// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Cloud/SecretsManagerUpload>
#include <Mib/Cloud/SecretsManagerDownload>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/CommandLine/TableRenderer>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_SecretsManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		auto SecretsManagerHost = "SecretsManagerHost?"_o=
			{
				"Names"_o= _o["--host"]
				, "Default"_o= ""
				, "Description"_o= "Limit query to only specified host ID."
			}
		;
		auto BinaryAsBase64 = "BinaryAsBase64"_o=
			{
				"Names"_o= _o["--binary-as-base64"]
				, "Default"_o= true
				, "Description"_o= "Binary secrets will be read and written as base64 encoded strings."
			}
		;
		auto Name = "Name?"_o=
			{
				"Names"_o= _o["--name"]
				, "Type"_o= ""
				, "Description"_o= "Limit query to secrets having the specified name. Wildcard search.\n"
			}
		;
		auto Tags = "Tags?"_o=
			{
				"Names"_o= _o["--tags"]
				, "Default"_o= _o[]
				, "Type"_o= _o[""]
				, "Description"_o= "Limit query to secrets having the specified tags.\n"
				"The tags are specified in a JSON array '[\"Tag1\", \"Tag2\" ...]' and the tags must adhere to RFC 1123 (hostname)."
			}
		;
		auto Expect = "Expect?"_o=
			{
				"Names"_o= _o["--expect"]
				, "Type"_o= COneOf{"string", "binary", "file", "string_map"}
				, "Default"_o= "string"
				, "Description"_o= "Unless the secret is of the expected variant report an error.\n"
			}
		;
		auto MapKey = "MapKey?"_o=
			{
				"Names"_o= _o["--map-key"]
				, "Type"_o= ""
				, "Description"_o= "For map secrets, index with specified key and return the value for this key."
			}
		;
		auto IDParameter = "Parameters"_o=
			{
				"ID"_o=
				{
					"Type"_o= ""
					, "Description"_o= "Specify secret ID.\n"
					"The ID is specified as Folder/Name with folder and name adhering to RFC 1123 (hostname). Additionally for Folder / is allowed, and for Name # is allowed."
				}
			}
		;
		auto CurrentDirectory = "CurrentDirectory?"_o=
			{
				"Names"_o= _o[]
				, "Default"_o= CFile::fs_GetCurrentDirectory()
				, "Hidden"_o= true
				, "Description"_o= "Internal hidden option to forward current directory."
			}
		;
		auto Quiet = "Quiet?"_o=
			{
				"Names"_o= _o["--quiet"]
				, "Default"_o= false
				, "Description"_o= "Suppress non-error output."
			}
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--secrets-manager-enumerate-secrets"]
					, "Description"_o= "List the IDs of all secrets in the database."
					, "Options"_o=
					{
						SecretsManagerHost
						, "SemanticID?"_o=
						{
							"Names"_o= _o["--semantic-id"]
							, "Default"_o= ""
							, "Description"_o= "Limit query to secrets having the specified semantic ID wildcard.\n"
							"The semantic ID must adhere to RFC 1123 (hostname) and additionally # is allowed as well as * and ? for wildcard."
						}
						, Name
						, Tags
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_EnumerateSecrets(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--secrets-manager-get-secret-by-semantic-id"]
					, "Description"_o= "Get secret matching the semantic id and tags."
					, "Options"_o=
					{
						SecretsManagerHost
						, Name
						, Tags
						, Expect
						, BinaryAsBase64
						, MapKey
					}
					, "Parameters"_o=
					{
						"SemanticID"_o=
						{
							"Type"_o= ""
							, "Description"_o= "Get the secret with the specified semantic ID.\n"
							"The semantic ID must adhere to RFC 1123 (hostname) and additionally # is allowed."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_GetSecretBySemanticID(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--secrets-manager-get-secret-properties"]
					, "Description"_o= "List all properties for the secret."
					, "Options"_o=
					{
						SecretsManagerHost
						, BinaryAsBase64
						, CTableRenderHelper::fs_OutputTypeOption()
					}
					, IDParameter
				}
				, [this](CEJsonSorted &&_Params, TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_GetProperties(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--secrets-manager-get-secret"]
					, "Description"_o= "Get secret."
					, "Output"_o= "The secret formatted acconding to --expect."
					, "Options"_o=
					{
						SecretsManagerHost
						, Expect
						, BinaryAsBase64
						, MapKey
					}
					, IDParameter
				}
				, [this](CEJsonSorted &&_Params, TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_GetSecret(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--secrets-manager-set-secret-properties"]
					, "Description"_o= "Set properties for a secret.\n"
					"Add a new secret or change the properties of an existing secret.\n"
					"When creating a new secret, properties that are not set on the command line will be assigned default values. "
					"The default values will be empty except for the Created and Modified properties which will be set to the current time.\n"
					"When an existing secret is modified, properties not specified on the command line will retain their previous values, except the Modified property.\n"
					, "Options"_o=
					{
						SecretsManagerHost
						, "Secret?"_o=
						{
							"Names"_o= _o["--secret"]
							, "Type"_o= COneOf{"string", "binary", "string_map", "binary_map"}
							, "Description"_o= "Set the secret.\n"
							"The secret can be a string secret or a binary secret.\n"
							"Specify 'string' to be prompted for a string secret.\n"
							"Specify 'binary' for a binary secret. When --binary-as-base64 is enabled (which it is by default) you will be prompted for a base64 encoded binary secret.\n"
							"Specify 'string_map' to be prompted for a map of string secrets.\n"
							"Specify 'binary_map' to be prompted for a map of binary secrets.\n"
							"When --binary-as-base64 is disabled, the raw binary secret must be piped or redirected to stdin.\n"
							"To set a file secret use --secrets-manager-upload-file."
						}
						, "Username?"_o=
						{
							"Names"_o= _o["--username"]
							, "Type"_o= ""
							, "Description"_o= "The username to set.\n"
						}
						, "URL?"_o=
						{
							"Names"_o= _o["--URL"]
							, "Type"_o= ""
							, "Description"_o= "The URL to set.\n"
						}
						, "Expires?"_o=
						{
							"Names"_o= _o["--expires"]
							, "Type"_o= CTime()
							, "Description"_o= "The time when the secret expires.\n"
						}
						, "Notes?"_o=
						{
							"Names"_o= _o["--notes"]
							, "Type"_o= ""
							, "Description"_o= "The notes to set.\n"
						}
						, "Metadata?"_o=
						{
							"Names"_o= _o["--metadata"]
							, "Type"_o= _o={}
							, "Description"_o= "The metadata to set.\n"
							"The metadata is specified as a JSON object '{\"Key\" : \"Value\" ...}'."
						}
						, "Created?"_o=
						{
							"Names"_o= _o["--created"]
							, "Type"_o= CTime()
							, "Description"_o= "The time when the secret was created. Set automatically if not specified.\n"
						}
						, "Modified?"_o=
						{
							"Names"_o= _o["--modified"]
							, "Type"_o= CTime()
							, "Description"_o= "The time when the secret was modified. Set automatically if not specified.\n"
						}
						, "Tags?"_o=
						{
							"Names"_o= _o["--tags"]
							, "Type"_o= _o[""]
							, "Description"_o= "The tags to set.\n"
							"The tags are specified in a JSON array '[\"Tag1\", \"Tag2\" ...]' and the tags must adhere to RFC 1123 (hostname)."
						}
						, "SemanticID?"_o=
						{
							"Names"_o= _o["--semantic-id"]
							, "Type"_o= ""
							, "Description"_o= "The semantic ID to set.\n"
							"The semantic ID must adhere to RFC 1123 (hostname) and additionally # is allowed."
						}
						, BinaryAsBase64
					}
					, IDParameter
				}
				, [this](CEJsonSorted &&_Params, TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_SetProperties(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--secrets-manager-change-tags"]
					, "Description"_o= "Remove or add tags from/to a secret."
					, "Options"_o=
					{
						SecretsManagerHost
						, "RemoveTags?"_o=
						{
							"Names"_o= _o["--remove"]
							, "Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "Remove these tags.\n"
							"The tags are specified in a JSON array '[\"Tag1\", \"Tag2\" ...]' and the tags must adhere to RFC 1123 (hostname)."
						}
						, "AddTags?"_o=
						{
							"Names"_o= _o["--add"]
							, "Default"_o= _o[]
							, "Type"_o= _o[""]
							, "Description"_o= "Add these tags.\n"
							"The tags are specified in a JSON array '[\"Tag1\", \"Tag2\" ...]' and the tags must adhere to RFC 1123 (hostname)."
						}
					}
					, IDParameter
				}
				, [this](CEJsonSorted &&_Params, TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_ChangeTags(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--secrets-manager-set-metadata"]
					, "Description"_o= "Set or add metadata to a secret.\n"
					"Add a new key, value pair to the secrets metadata or replace the value if the key already exists."
					, "Options"_o=
					{
						SecretsManagerHost
						, "Key"_o=
						{
							"Names"_o= _o["--key"]
							, "Type"_o= ""
							, "Description"_o= "The key to set metadata for.\n"
						}
						, "Value"_o=
						{
							"Names"_o= _o["--value"]
							, "Type"_o= fg_AnyType()
							, "Description"_o= "The value to for the metadata.\n"
						}
						, "ExpectedValue?"_o=
						{
							"Names"_o= _o["--expected-value"]
							, "Type"_o= fg_AnyType()
							, "Description"_o= "The expected value to replace.\nIf the value is different the operation will fail.\n"
						}
					}
					, IDParameter
				}
				, [this](CEJsonSorted &&_Params, TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_SetMetadata(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--secrets-manager-remove-metadata"]
					, "Description"_o= "Remove the metadata matching key from the secret."
					, "Options"_o=
					{
						SecretsManagerHost
						, "Key"_o=
						{
							"Names"_o= _o["--key"]
							, "Type"_o= ""
							, "Description"_o= "Key of the metadata to remove.\n"
						}
					}
					, IDParameter
				}
				, [this](CEJsonSorted &&_Params, TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_RemoveMetadata(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--secrets-manager-remove-secret"]
					, "Description"_o= "Remove the secret."
					, "Options"_o=
					{
						SecretsManagerHost
					}
					, IDParameter
				}
				, [this](CEJsonSorted &&_Params, TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_RemoveSecret(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--secrets-manager-upload-file"]
					, "Description"_o= "Upload a file."
					, "Options"_o=
					{
						SecretsManagerHost
						, "SecretFile"_o=
						{
							"Names"_o= _o["--secret-file"]
							, "Type"_o= ""
							, "Description"_o= "The secret file to set.\n"
						}
						, Quiet
						, CurrentDirectory
					}
					, IDParameter
				}
				, [this](CEJsonSorted &&_Params, TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_Upload(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--secrets-manager-download-file"]
					, "Description"_o= "Download a file."
					, "Options"_o=
					{
						SecretsManagerHost
						, "OutputFile?"_o=
						{
							"Names"_o= _o["--output-file"]
							, "Type"_o= ""
							, "Description"_o= "Filename for the downloaded file.\n"
						}
						, "AllowOverwrite?"_o=
						{
							"Names"_o= _o["--allow-overwrite"]
							, "Default"_o= false
							, "Description"_o= "Allow overwirte of destination file.\n"
							"Only valid when output file is specified, otherwise file overwrite is never allowed.\n"
						}
						, Quiet
						, CurrentDirectory
					}
					, IDParameter
				}
				, [this](CEJsonSorted &&_Params, TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SecretsManager_Download(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}

	bool CCloudClientAppActor::fsp_SecretsManager_GetID(CEJsonSorted const &_Params, CSecretsManager::CSecretID &o_ID, CStr &o_Error)
	{
		if (auto const &ID = _Params["ID"].f_String())
		{
			aint SeparatorPosition = fg_StrFindCharReverse(ID, '/');
			if (SeparatorPosition > 0)
			{
				o_ID = {ID.f_Left(SeparatorPosition), ID.f_Extract(SeparatorPosition + 1)};
				if (!CSecretsManager::fs_IsValidFolder(o_ID.m_Folder))
				{
					o_Error = fg_Format("'{}' is not a valid Folder", o_ID.m_Folder);
					return true;
				}
				if (!CSecretsManager::fs_IsValidName(o_ID.m_Name))
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

	CStr CCloudClientAppActor::fsp_SecretsManager_CheckExpectedFormat
		(
			CSecretsManager::CSecret const &_Secret
			, NStr::CStr const &_ExpectedFormat
			, bool _bBinaryAsBase64
			, TCOptional<CStrSecure> const &_MapKey
		)
	{
		if (_ExpectedFormat)
		{
			auto SecretType = _Secret.f_GetTypeID();

			if (SecretType == CSecretsManager::ESecretType_BinaryMap)
			{
				if (!_bBinaryAsBase64)
					return "Binary map secrets cannot be emitted in raw binary form. Use --binary-as-base64 to emit secrets.";
			}

			if (_MapKey)
			{
				if (_ExpectedFormat == "string_map")
					return "When --map-key is used output cannot be emitted as a string_map";

				if (SecretType != CSecretsManager::ESecretType_StringMap && SecretType != CSecretsManager::ESecretType_BinaryMap)
					return "When --map-key is specified secret needs to be a string map or binary map";

				if (SecretType == CSecretsManager::ESecretType_StringMap)
					SecretType = CSecretsManager::ESecretType_String;
				else if (SecretType == CSecretsManager::ESecretType_BinaryMap)
					SecretType = CSecretsManager::ESecretType_Binary;
			}

			if (_ExpectedFormat == "string")
			{
				if (SecretType != CSecretsManager::ESecretType_String && SecretType != CSecretsManager::ESecretType_Binary)
					return "Only string secrets and binary secrets can be emitted in string form";
			}
			else if (_ExpectedFormat == "string_map")
			{
				if (SecretType != CSecretsManager::ESecretType_StringMap && SecretType != CSecretsManager::ESecretType_BinaryMap)
					return "Only string map secrets and binary map secrets can be emitted in string map form";
			}
			else if (_ExpectedFormat == "binary")
			{
				if (SecretType != CSecretsManager::ESecretType_Binary)
					return "Only binary secrets can be emitted in binary form";
				if (_bBinaryAsBase64)
					return "Binary secrets cannot be emitted in raw binary form when --binary-as-base64 is enabled. Use --no-binary-as-base64 to emit secrets in raw binary form.";
			}
			else if (_ExpectedFormat == "file")
			{
				if (SecretType != CSecretsManager::ESecretType_File)
					return "Only file secrets can be emitted in file form";
			}
		}
		return {};
	}

	TCFuture<void> CCloudClientAppActor::fp_SecretsManager_SubscribeToServers()
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		if (!mp_SecretsManagers.f_IsEmpty())
			co_return {};

		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to secrets managers");

		auto Subscription = co_await mp_State.m_TrustManager->f_SubscribeTrustedActors<NCloud::CSecretsManager>().f_Wrap();

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

	template <typename tf_CType>
	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_EnumerateImpl
		(
			CEJsonSorted const _Params
			, TCSharedPointer<CCommandLineControl> _pCommandLine
			, TCFunctionMovable
			<
				TCFuture<tf_CType>
				(
					TCDistributedActor<CSecretsManager> const &_Actor
					, TCOptional<CStrSecure> const &_SemanticID
					, TCOptional<CStrSecure> const &_Name
					, TCSet<CStrSecure> const &_Tags
				)
			> _fGetResult
			, TCFunctionMovable
			<
				NStr::CStr
				(
					tf_CType const &_Result
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CStr const &_ExpectedFormat
					, TCOptional<CStrSecure> const &_MapKey
					, bool _bBinaryAsBase64
				)
			> _fOnResult
		)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

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
			if (!CSecretsManager::fs_IsValidSemanticIDWildcard(ID))
				co_return DMibErrorInstance(fg_Format("'{}' is not a valid Semantic ID", ID));
			SemanticID = ID;
		}

		TCOptional<CStrSecure> MapKey;
		if (auto pID = _Params.f_GetMember("MapKey"))
			MapKey = pID->f_String();

		TCOptional<CStrSecure> Name;
		if (auto pID = _Params.f_GetMember("Name"))
		{
			auto &ID = pID->f_String();
			if (!CSecretsManager::fs_IsValidSemanticIDWildcard(ID))
				co_return DMibErrorInstance(fg_Format("'{}' is not a valid Name ID", ID));
			Name = ID;
		}

		TCSet<CStrSecure> Tags;
		for (auto &TagJson : _Params["Tags"].f_Array())
		{
			CStr const &Tag = TagJson.f_String();
			if (!CSecretsManager::fs_IsValidTag(Tag))
				co_return DMibErrorInstance(fg_Format("'{}' is not a valid tag", Tag));

			Tags[Tag];
		}

		co_await fp_SecretsManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		TCFutureMap<CHostInfo, tf_CType> Secrets;

		for (auto &TrustedActor : mp_SecretsManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			_fGetResult(TrustedActor.m_Actor, SemanticID, Name, Tags).f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
				> Secrets[TrustedActor.m_TrustInfo.m_HostInfo]
			;
		}

		TCMap<CHostInfo, TCAsyncResult<tf_CType>> Results = co_await fg_AllDoneWrapped(Secrets);

		tf_CType ResultToReturn;
		bool bFirst = true;

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

			if (bFirst)
			{
				bFirst = false;
				ResultToReturn = *Result;
			}
			else if (ResultToReturn != *Result)
			{
				if constexpr (cIsSame<tf_CType, TCSet<CSecretsManager::CSecretID>>)
					ResultToReturn += *Result;
				else if constexpr (cIsSame<tf_CType, CSecretsManager::CSecret>)
					co_return DMibErrorInstance("Data inconsistency between secrets managers for '{}'. Please try again later.\n{}\n!=\n{}\n"_f << ResultToReturn << *Result);
				else
					static_assert(gc_MakeValueDependent<false, tf_CType>, "Missing support");
			}
		}

		if (!bFirst)
		{
			if (auto ErrorStr = _fOnResult(ResultToReturn, _pCommandLine, Expect, MapKey, bBinaryAsBase64))
				co_return DMibErrorInstance(ErrorStr);
		}
		else
			co_return DMibErrorInstance("No secrets found on any connected secret manager");

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_EnumerateSecrets(CEJsonSorted const _Params, TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		return fp_CommandLine_SecretsManager_EnumerateImpl<TCSet<CSecretsManager::CSecretID>>
			(
				fg_Move(fg_RemoveQualifiers(_Params))
				, fg_Move(_pCommandLine)
				, [](TCDistributedActor<CSecretsManager> const &_Actor, TCOptional<CStrSecure> const &_SemanticID, TCOptional<CStrSecure> const &_Name, TCSet<CStrSecure> const &_Tags)
				-> TCFuture<TCSet<CSecretsManager::CSecretID>>
				{
					CSecretsManager::CEnumerateSecrets Options;
					Options.m_SemanticID = _SemanticID;
					Options.m_Name = _Name;
					Options.m_TagsExclusive = _Tags;

					return _Actor.f_CallActor(&CSecretsManager::f_EnumerateSecrets)(fg_Move(Options));
				}
				, []
				(
					TCSet<CSecretsManager::CSecretID> const &_Result
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CStr const &_ExpectedFormat
					, TCOptional<CStrSecure> const &_MapKey
					, bool _bBinaryAsBase64
				) -> CStr
				{
					for (auto &ID : _Result)
						*_pCommandLine += "{}\n"_f << ID;

					return "";
				}
			)
		;

	}

	NStr::CStr CCloudClientAppActor::fsp_SecretsManager_OutputSecret
		(
			TCSharedPointer<CCommandLineControl> const &_pCommandLine
			, CSecretsManager::CSecret const &_Secret
			, bool _bBinaryAsBase64
			, TCOptional<CStrSecure> const &_MapKey
		)
	{
		if (_MapKey)
		{
			auto &MapKey = *_MapKey;

			if (_Secret.f_GetTypeID() == CSecretsManager::ESecretType_BinaryMap)
			{
				DMibCheck(_bBinaryAsBase64);

				auto &SecretsMap = _Secret.f_Get<CSecretsManager::ESecretType_BinaryMap>();
				auto *pSecret = SecretsMap.f_FindEqual(MapKey);
				if (!pSecret)
					return "No secret with name '{}' exists in binary map"_f << MapKey;

				if (!_bBinaryAsBase64)
					*_pCommandLine += *pSecret;
				else
					*_pCommandLine += "{}\n"_f << NEncoding::fg_Base64Encode(*pSecret);

				return {};
			}
			else if (_Secret.f_GetTypeID() == CSecretsManager::ESecretType_StringMap)
			{
				auto &SecretsMap = _Secret.f_Get<CSecretsManager::ESecretType_StringMap>();
				auto *pSecret = SecretsMap.f_FindEqual(MapKey);
				if (!pSecret)
					return "No secret with name '{}' exists in string map"_f << MapKey;

				*_pCommandLine += "{}\n"_f << *pSecret;

				return {};
			}
			else
				DMibNeverGetHere;
		}

		if (_Secret.f_GetTypeID() == CSecretsManager::ESecretType_Binary && !_bBinaryAsBase64)
			*_pCommandLine += _Secret.f_Get<CSecretsManager::ESecretType_Binary>();
		else
			*_pCommandLine += "{}\n"_f << _Secret;

		return {};
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_GetSecretBySemanticID
		(
			CEJsonSorted const _Params
			, TCSharedPointer<CCommandLineControl> _pCommandLine
		)
	{
		return fp_CommandLine_SecretsManager_EnumerateImpl<CSecretsManager::CSecret>
			(
				fg_Move(fg_RemoveQualifiers(_Params))
				, fg_Move(_pCommandLine)
				, [](TCDistributedActor<CSecretsManager> const &_Actor, TCOptional<CStrSecure> const &_SemanticID, TCOptional<CStrSecure> const &_Name, TCSet<CStrSecure> const &_Tags)
				-> TCFuture<CSecretsManager::CSecret>
				{
					CSecretsManager::CGetSecretBySemanticID Options;
					Options.m_SemanticID = *_SemanticID;
					Options.m_Name = _Name;
					Options.m_TagsExclusive = _Tags;

					return _Actor.f_CallActor(&CSecretsManager::f_GetSecretBySemanticID)(fg_Move(Options));
				}
				, []
				(
					CSecretsManager::CSecret const &_Secret
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CStr const &_ExpectedFormat
					, TCOptional<CStrSecure> const &_MapKey
					, bool _bBinaryAsBase64
				) -> CStr
				{
					if (auto Error = fsp_SecretsManager_CheckExpectedFormat(_Secret, _ExpectedFormat, _bBinaryAsBase64, _MapKey))
						return Error;

					if (auto Error = fsp_SecretsManager_OutputSecret(_pCommandLine, _Secret, _bBinaryAsBase64, _MapKey))
						return Error;

					return {};
				}
			)
		;

	}

	template<typename tf_CType>
	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_GetImpl
		(
			CEJsonSorted const _Params
			, TCSharedPointer<CCommandLineControl> _pCommandLine
			, TCFunctionMovable<TCFuture<tf_CType> (TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID)> _fGetResult
			, TCFunctionMovable
			<
				NStr::CStr
				(
					tf_CType const &_Result
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CStr const &_ExpectedFormat
					, TCOptional<CStrSecure> const &_MapKey
					, bool _bBinaryAsBase64
				)
			> _fOnResult
		)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

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

		TCOptional<CStrSecure> MapKey;
		if (auto pID = _Params.f_GetMember("MapKey"))
			MapKey = pID->f_String();

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

		co_await fp_SecretsManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		TCFutureMap<CHostInfo, tf_CType> Secrets;

		for (auto &TrustedActor : mp_SecretsManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			_fGetResult(TrustedActor.m_Actor, ID).f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
				> Secrets[TrustedActor.m_TrustInfo.m_HostInfo]
			;
		}

		TCMap<CHostInfo, TCAsyncResult<tf_CType>> Results = co_await fg_AllDoneWrapped(Secrets);

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
				co_return DMibErrorInstance("Data inconsistency between secrets managers. Please try again later.\n{}\n!=\n{}\n"_f << *pFirstResult << *Result);
		}

		if (pFirstResult)
		{
			if (auto ErrorStr = _fOnResult(*pFirstResult, _pCommandLine, Expect, MapKey, bBinaryAsBase64))
				co_return DMibErrorInstance(ErrorStr);
		}
		else
			co_return DMibErrorInstance("No secrets found on any connected secret manager");

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_GetSecret(CEJsonSorted const _Params, TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		return fp_CommandLine_SecretsManager_GetImpl<CSecretsManager::CSecret>
			(
				fg_Move(fg_RemoveQualifiers(_Params))
				, fg_Move(_pCommandLine)
				, [](TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID) -> TCFuture<CSecretsManager::CSecret>
				{
					return _Actor.f_CallActor(&CSecretsManager::f_GetSecret)(fg_TempCopy(_ID));
				}
				, []
				(
					CSecretsManager::CSecret const &_Secret
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CStr const &_ExpectedFormat
					, TCOptional<CStrSecure> const &_MapKey
					, bool _bBinaryAsBase64
				) -> CStr
				{
					if (auto Error = fsp_SecretsManager_CheckExpectedFormat(_Secret, _ExpectedFormat, _bBinaryAsBase64, _MapKey))
						return Error;

					if (auto Error = fsp_SecretsManager_OutputSecret(_pCommandLine, _Secret, _bBinaryAsBase64, _MapKey))
						return Error;

					return {};
				}
			)
		;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_GetProperties(CEJsonSorted const _Params, TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		return fp_CommandLine_SecretsManager_GetImpl<CSecretsManager::CSecretProperties>
			(
				fg_Move(fg_RemoveQualifiers(_Params))
				, fg_Move(_pCommandLine)
				, [](TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID) -> TCFuture<CSecretsManager::CSecretProperties>
				{
					return _Actor.f_CallActor(&CSecretsManager::f_GetSecretProperties)(fg_TempCopy(_ID));
				}
				, []
				(
					CSecretsManager::CSecretProperties const &_Result
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, CStr const &_ExpectedFormat
					, TCOptional<CStrSecure> const &_MapKey
					, bool _bBinaryAsBase64
				) -> CStr
				{
					if ((*_Result.m_Secret).f_GetTypeID() == CSecretsManager::ESecretType_File)
						*_pCommandLine += "Secret (file):   {}\n"_f << *_Result.m_Secret;
					else if ((*_Result.m_Secret).f_GetTypeID() == CSecretsManager::ESecretType_Binary && !_bBinaryAsBase64)
						*_pCommandLine += "Secret (base64): {}\n"_f << *_Result.m_Secret;
					else
						*_pCommandLine += "Secret:          {}\n"_f << *_Result.m_Secret;

					if (*_Result.m_UserName)
						*_pCommandLine += "Username:        {}\n"_f << *_Result.m_UserName;

					if (*_Result.m_URL)
						*_pCommandLine += "URL:             {}\n"_f << *_Result.m_URL;

					if ((*_Result.m_Expires).f_IsValid())
						*_pCommandLine += "Expires:         {}\n"_f << *_Result.m_Expires;

					if (*_Result.m_Notes)
						*_pCommandLine += "Notes:           {}\n"_f << *_Result.m_Notes;

					if (!(*_Result.m_Metadata).f_IsEmpty())
						*_pCommandLine += "Metadata:\n{}"_f << *_Result.m_Metadata;

					if ((*_Result.m_Created).f_IsValid())
						*_pCommandLine += "Created:         {}\n"_f << *_Result.m_Created;

					if ((*_Result.m_Modified).f_IsValid())
						*_pCommandLine += "Modified:        {}\n"_f << *_Result.m_Modified;

					if (*_Result.m_SemanticID)
						*_pCommandLine += "SemanticID:      {}\n"_f << *_Result.m_SemanticID;

					if (!(*_Result.m_Tags).f_IsEmpty())
						*_pCommandLine += "Tags:            {vs}\n"_f << *_Result.m_Tags;

					if (_Result.m_Immutable && *_Result.m_Immutable)
						*_pCommandLine += "Immutable:            true\n";

					return "";
				}
			)
		;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_SetProperties(CEJsonSorted const _Params, TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

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
			for (auto iMetadata = Object.f_OrderedIterator(); iMetadata; ++iMetadata)
				Properties.f_SetMetadata(iMetadata->f_Name(), fg_TempCopy(iMetadata->f_Value()));
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

			if (!CSecretsManager::fs_IsValidSemanticID(ID))
				co_return DMibErrorInstance(fg_Format("'{}' is not a valid SemanticID", ID));

			Properties.f_SetSemanticID(ID);
			++nSetProperties;
		}

		if (auto pValue = _Params.f_GetMember("Tags"))
		{
			TCSet<CStrSecure> Tags;
			for (auto &TagJson : pValue->f_Array())
			{
				CStr const &Tag = TagJson.f_String();

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
					{
						auto CaptureScope = co_await (g_CaptureExceptions % "Base64 decoding failed");

						CDisableExceptionTraceScope Disabled;
						fg_Base64Decode(Encoded, Decoded);
					}

					Secret = CSecretsManager::CSecret{fg_Move(Decoded)};
				}
				else
					Secret = CSecretsManager::CSecret{(co_await _pCommandLine->f_ReadBinary()).f_ToSecure()};
			}
			else
				DNeverGetHere;
		}

		if (nSetProperties == 0)
			co_return DMibErrorInstance("No properties specified. Specify at least one property to change");

		if (SecretWasSet)
			Properties.f_SetSecret(fg_Move(Secret));

		co_await fp_SecretsManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		auto Result = co_await pSecretsManager->m_Actor.f_CallActor(&CSecretsManager::f_SetSecretProperties)(fg_Move(ID), fg_Move(Properties))
			.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
		;

		if (!(Result.m_Flags & CSecretsManager::ESetSecretPropertiesResultFlag_Updated))
			*_pCommandLine %= "No changes detected. Secret was not updated\n";

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_ChangeTags(CEJsonSorted const _Params, TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

		auto fParseTags = [](CEJsonSorted const &_Tags)
			{
				TCSet<CStrSecure> OutTags;
				for (auto &TagJson : _Tags.f_Array())
				{
					CStr const &Tag = TagJson.f_String();
					if (!CVersionManager::fs_IsValidTag(Tag))
						DMibError(fg_Format("'{}' is not a valid tag", Tag));
					OutTags[Tag];
				}
				return OutTags;
			}
		;

		TCSet<CStrSecure> AddTags;
		TCSet<CStrSecure> RemoveTags;

		{
			auto CaptureScope = co_await g_CaptureExceptions;

			AddTags = fParseTags(_Params["AddTags"]);
			RemoveTags = fParseTags(_Params["RemoveTags"]);
		}

		if (AddTags.f_IsEmpty() && RemoveTags.f_IsEmpty())
			co_return DMibErrorInstance("No changes specified. Specify tags to add and remove with --add and --remove");

		co_await fp_SecretsManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		co_await pSecretsManager->m_Actor.f_CallActor(&CSecretsManager::f_ModifyTags)(fg_Move(ID), fg_Move(RemoveTags), fg_Move(AddTags))
			.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
		;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_SetMetadata(CEJsonSorted const _Params, TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

		co_await fp_SecretsManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		CSecretsManager::CSetMetadata SetMetadata;
		SetMetadata.m_ID = ID;
		SetMetadata.m_Key = _Params["Key"].f_String();
		SetMetadata.m_Value = _Params["Value"];
		if (auto *pExpectedValue = _Params.f_GetMember("ExpectedValue"))
			SetMetadata.m_ExpectedValue = *pExpectedValue;

		co_await pSecretsManager->m_Actor.f_CallActor(&CSecretsManager::f_SetMetadata)(fg_Move(SetMetadata)).f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply");

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_RemoveMetadata(CEJsonSorted const _Params, TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

		CStrSecure Key;
		if (auto pValue = _Params.f_GetMember("Key"))
			Key = pValue->f_String();

		co_await fp_SecretsManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		co_await pSecretsManager->m_Actor.f_CallActor(&CSecretsManager::f_RemoveMetadata)(fg_Move(ID), Key)
			.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
		;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_RemoveSecret(CEJsonSorted const _Params, TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

		CStr Host = _Params["SecretsManagerHost"].f_String();
		CSecretsManager::CSecretID ID;
		CStr Error;

		if (fsp_SecretsManager_GetID(_Params, ID, Error))
			co_return DMibErrorInstance(Error);

		co_await fp_SecretsManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

		auto *pSecretsManager = mp_SecretsManagers.f_GetOneActor(Host, Error);
		if (!pSecretsManager)
			co_return DMibErrorInstance(fg_Format("Error selecting secrets manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		co_await pSecretsManager->m_Actor.f_CallActor(&CSecretsManager::f_RemoveSecret)(fg_Move(ID))
			.f_Timeout(mp_Timeout, "Timed out waiting for secrets manager to reply")
		;

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_Upload(CEJsonSorted const _Params, TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

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

		co_await fp_SecretsManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

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
					, fg_Reference(mp_UploadSubscription)
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

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_SecretsManager_Download(CEJsonSorted const _Params, TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CheckDestroy = co_await fp_CheckStoppedOrDestroyedOnResume();

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

		co_await fp_SecretsManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for secrets managers");

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
