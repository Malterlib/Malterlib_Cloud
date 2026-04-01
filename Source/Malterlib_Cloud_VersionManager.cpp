// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>

#include "Malterlib_Cloud_VersionManager.h"

namespace NMib::NCloud
{
	using namespace NStr;

	CVersionManager::CVersionManager()
	{
		DMibPublishActorFunction(CVersionManager::f_ListApplications);
		DMibPublishActorFunction(CVersionManager::f_ListVersions);
		DMibPublishActorFunction(CVersionManager::f_UploadVersion);
		DMibPublishActorFunction(CVersionManager::f_DownloadVersion);
		DMibPublishActorFunction(CVersionManager::f_SubscribeToUpdates);
		DMibPublishActorFunction(CVersionManager::f_ChangeTags);
	}

	DMibDistributedStreamImplement(CVersionManager::CDownloadFile);
	DMibDistributedStreamImplement(CVersionManager::CVersionID);
	DMibDistributedStreamImplement(CVersionManager::CVersionIDAndPlatform);
	DMibDistributedStreamImplement(CVersionManager::CVersionInformation);
	DMibDistributedStreamImplement(CVersionManager::CListApplications);
	DMibDistributedStreamImplement(CVersionManager::CListApplications::CResult);
	DMibDistributedStreamImplement(CVersionManager::CListVersions);
	DMibDistributedStreamImplement(CVersionManager::CListVersions::CResult);
	DMibDistributedStreamImplement(CVersionManager::CStartUploadTransferDeprecated);
	DMibDistributedStreamImplement(CVersionManager::CStartUploadTransferDeprecated::CResult);
	DMibDistributedStreamImplement(CVersionManager::CStartUploadVersion);
	DMibDistributedStreamImplement(CVersionManager::CStartUploadVersion::CResult);
	DMibDistributedStreamImplement(CVersionManager::CStartDownloadVersion);
	DMibDistributedStreamImplement(CVersionManager::CStartDownloadVersion::CResult);
	DMibDistributedStreamImplement(CVersionManager::CNewVersionNotification);
	DMibDistributedStreamImplement(CVersionManager::CNewVersionNotifications);
	DMibDistributedStreamImplement(CVersionManager::CNewVersionNotifications::CResult);
	DMibDistributedStreamImplement(CVersionManager::CSubscribeToUpdates);
	DMibDistributedStreamImplement(CVersionManager::CSubscribeToUpdates::CResult);
	DMibDistributedStreamImplement(CVersionManager::CChangeTags);
	DMibDistributedStreamImplement(CVersionManager::CChangeTags::CResult);

	CVersionManager::~CVersionManager() = default;

	NStr::CStr CVersionManager::CVersionIDAndPlatform::fs_ConvertFromOldPlatform(NStr::CStr const &_Platform)
	{
		return _Platform.f_Replace("Linux2.6", "Linux").f_Replace("OSX10.7", "macOS");
	}

	bool CVersionManager::fs_IsValidApplicationName(CStr const &_String)
	{
		return NNetwork::fg_IsValidHostname(_String);
	}

	bool CVersionManager::fs_IsValidBranch(NStr::CStr const &_String, bool _bAllowWildCards)
	{
		if (_String.f_IsEmpty())
			return false;

		if (_String.f_StartsWith("."))
			return false;

		if (_String.f_Find("/.") >= 0)
			return false;

		if (_String.f_EndsWith("/"))
			return false;

		if (_String.f_EndsWith(".lock"))
			return false;

		if (_bAllowWildCards)
		{
			if (_String.f_FindChars("~^:[]\\ \t\b\r\n") >= 0)
				return false;
		}
		else
		{
			if (_String.f_FindChars("*?~^:[]\\ \t\b\r\n") >= 0)
				return false;
		}

		return true;
	}

	bool CVersionManager::fs_IsValidTag(CStr const &_String)
	{
		return NNetwork::fg_IsValidHostname(_String);
	}

	bool CVersionManager::fs_IsValidProtocolVersion(uint32 _Version)
	{
		return _Version >= EProtocolVersion_Min && _Version <= EProtocolVersion_Current;
	}

	bool CVersionManager::fs_IsValidVersionIdentifier(CVersionID const &_VersionID, CStr &o_Error)
	{
		if (!fs_IsValidBranch(_VersionID.m_Branch))
		{
			o_Error = "Branch is not valid";
			return false;
		}
		return true;
	}

	bool CVersionManager::fs_IsValidVersionIdentifier(CStr const &_String, CStr &o_Error, CVersionID *o_pVersionID)
	{
		CVersionID VersionID;
		aint iBranchEnd = _String.f_FindCharReverse('/');
		if (iBranchEnd < 0)
		{
			o_Error = "Branch was not found";
			return false;
		}
		VersionID.m_Branch = _String.f_Left(iBranchEnd);
		CStr VersionString = _String.f_Extract(iBranchEnd + 1);
		aint nParsed = 0;
		aint nChars = (CStr::CParse("{}.{}.{}") >> VersionID.m_Major >> VersionID.m_Minor >> VersionID.m_Revision).f_Parse(VersionString, nParsed);
		if (nParsed != 3)
		{
			o_Error = "Did not find all three version parts (Major, Minor and Revision)";
			return false;
		}
		if (nChars != VersionString.f_GetLen())
		{
			o_Error = "Extra characters after version";
			return false;
		}
		if (VersionID.m_Branch.f_IsEmpty())
		{
			o_Error = "Branch is empty";
			return false;
		}
		if (!fs_IsValidVersionIdentifier(VersionID, o_Error))
			return false;
		if (o_pVersionID)
			*o_pVersionID = VersionID;
		return true;
	}

	bool CVersionManager::fs_IsValidPlatform(NStr::CStr const &_String)
	{
		return NNetwork::fg_IsValidHostname(_String);
	}

	// CVersionID

	CStr CVersionManager::CVersionID::f_EncodeFileName() const
	{
		return fg_Format("{}_{}.{}.{}", m_Branch.f_ReplaceChar('/', '_'), m_Major, m_Minor, m_Revision);
	}

	CStr CVersionManager::CVersionID::fs_DecodeFileName(CStr const &_FileName)
	{
		return _FileName.f_ReplaceChar('_', '/');
	}

	NEncoding::CEJsonSorted CVersionManager::CVersionID::f_ToJson() const
	{
		NEncoding::CEJsonSorted Ret;
		Ret["Branch"] = m_Branch;
		Ret["Major"] = m_Major;
		Ret["Minor"] = m_Minor;
		Ret["Revision"] = m_Revision;
		return Ret;
	}

	bool CVersionManager::CVersionID::f_IsValid() const
	{
		return !m_Branch.f_IsEmpty();
	}

	auto CVersionManager::CVersionID::fs_FromJson(NEncoding::CEJsonSorted const &_Json) -> CVersionID
	{
		CVersionID Ret;
		Ret.m_Branch = _Json["Branch"].f_String();
		Ret.m_Major = _Json["Major"].f_Integer();
		Ret.m_Minor = _Json["Minor"].f_Integer();
		Ret.m_Revision = _Json["Revision"].f_Integer();
		return Ret;
	}

	// CVersionIDAndPlatform

	CStr CVersionManager::CVersionIDAndPlatform::f_EncodeFileName() const
	{
		return fg_Format("{}/{}", m_VersionID.f_EncodeFileName(), m_Platform);
	}

	NEncoding::CEJsonSorted CVersionManager::CVersionIDAndPlatform::f_ToJson() const
	{
		NEncoding::CEJsonSorted Ret;
		Ret["VersionID"] = m_VersionID.f_ToJson();
		Ret["Platform"] = m_Platform;
		return Ret;
	}

	bool CVersionManager::CVersionIDAndPlatform::f_IsValid() const
	{
		return m_VersionID.f_IsValid() && !m_Platform.f_IsEmpty();
	}

	auto CVersionManager::CVersionIDAndPlatform::fs_FromJson(NEncoding::CEJsonSorted const &_Json) -> CVersionIDAndPlatform
	{
		CVersionIDAndPlatform Ret;
		Ret.m_VersionID = CVersionID::fs_FromJson(_Json["VersionID"]);
		Ret.m_Platform = _Json["Platform"].f_String();
		return Ret;
	}

	// CVersionInformation

	NEncoding::CEJsonSorted CVersionManager::CVersionInformation::f_ToJson() const
	{
		NEncoding::CEJsonSorted Ret;
		Ret["Time"] = m_Time;
		Ret["Configuration"] = m_Configuration;
		auto &TagArray = Ret["Tags"].f_Array();
		for (auto &Tag : m_Tags)
			TagArray.f_Insert(Tag);
		if (m_ExtraInfo.f_IsObject())
			Ret["ExtraInfo"] = m_ExtraInfo;
		Ret["NumFiles"] = m_nFiles;
		Ret["NumBytes"] = m_nBytes;
		Ret["RetrySequence"] = m_RetrySequence;
		return Ret;
	}

	auto CVersionManager::CVersionInformation::fs_FromJson(NEncoding::CEJsonSorted const &_Json) -> CVersionInformation
	{
		CVersionInformation Ret;
		Ret.m_Time = _Json["Time"].f_Date();
		Ret.m_Configuration = _Json["Configuration"].f_String();
		if (auto *pValue = _Json.f_GetMember("Tags", NEncoding::EJsonType_Array))
		{
			for (auto &Tag : pValue->f_Array())
				Ret.m_Tags[Tag.f_String()];
		}
		if (auto *pValue = _Json.f_GetMember("ExtraInfo", NEncoding::EJsonType_Object))
			Ret.m_ExtraInfo = *pValue;
		Ret.m_nFiles = _Json["NumFiles"].f_Integer();
		Ret.m_nBytes = _Json["NumBytes"].f_Integer();
		if (auto *pValue = _Json.f_GetMember("RetrySequence", NEncoding::EJsonType_Integer))
			Ret.m_RetrySequence = pValue->f_Integer();
		return Ret;
	}

	// CListApplications

	template <typename tf_CStream>
	void CVersionManager::CListApplications::CResult::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_Applications;
	}

	template <typename tf_CStream>
	void CVersionManager::CListApplications::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
	}

	// CListVersions

	template <typename tf_CStream>
	void CVersionManager::CListVersions::CResult::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_Versions;
	}

	template <typename tf_CStream>
	void CVersionManager::CListVersions::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_ForApplication;
	}

	// CStartUploadTransfer

	template <typename tf_CStream>
	void CVersionManager::CStartUploadTransferDeprecated::CResult::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % fg_Move(m_Subscription);
	}

	template <typename tf_CStream>
	void CVersionManager::CStartUploadTransferDeprecated::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_TransferContextDeprecated;
	}

	// CDownloadFileContents
	template <typename tf_CStream>
	void CVersionManager::CDownloadFileContents::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));

		_Stream % fg_Move(m_DataGenerator);
		_Stream % fg_Move(m_Subscription);
		_Stream % m_StartPosition;
	}

	// CDownloadFile

	template <typename tf_CStream>
	void CVersionManager::CDownloadFile::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_FilePath;
		_Stream % m_FileAttributes;
		_Stream % m_WriteTime;
		_Stream % m_FileSize;
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportSymlinks)
			_Stream % m_SymlinkContents;
		_Stream % fg_Move(m_fGetDataGenerator);
	}

	// CStartUploadVersion

	template <typename tf_CStream>
	void CVersionManager::CStartUploadVersion::CResult::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		if (_Stream.f_GetVersion() >= EProtocolVersion_RefactorToActorFunctorsUploadDownload)
			_Stream % fg_Move(m_fFinish);
		else
			_Stream % fg_Move(m_fFinish.f_GetSubscription());
		_Stream % m_DeniedTags;
	}

	template <typename tf_CStream>
	void CVersionManager::CStartUploadVersion::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_Application;
		_Stream % m_VersionIDAndPlatform;
		_Stream % m_VersionInfo;
		_Stream % m_QueueSize;
		_Stream % m_Flags;
		if (_Stream.f_GetVersion() >= EProtocolVersion_AsyncGeneratorFileTransfer)
		{
			if constexpr (tf_CStream::mc_Direction == NStream::EStreamDirection_Consume)
				m_FilesGenerator = fg_Construct();

			_Stream % fg_Move(*m_FilesGenerator);
		}
		else
		{
			if constexpr (tf_CStream::mc_Direction == NStream::EStreamDirection_Consume)
				m_fStartTransferDeprecated = fg_Construct();

			if (_Stream.f_GetVersion() >= EProtocolVersion_RefactorToActorFunctorsUploadDownload)
				_Stream % fg_Move(*m_fStartTransferDeprecated);
			else
			{
				if constexpr (tf_CStream::mc_Direction == NStream::EStreamDirection_Consume)
				{
					NConcurrency::TCActor<> DispatchActor;
					NFunction::TCFunctionMutable<NConcurrency::TCFuture<CStartUploadTransferDeprecated::CResult> (CStartUploadTransferDeprecated &&_Params)> fStartTransfer;
					_Stream.f_GetStream() >> DispatchActor;
					_Stream.f_GetStream() >> fStartTransfer;

					m_fStartTransferDeprecated = {fg_Move(DispatchActor), fg_Move(fStartTransfer)};
				}
				else
				{
					_Stream.f_GetStream() << fg_Move(m_fStartTransferDeprecated->f_GetActor());
					_Stream.f_GetStream() << fg_Move(m_fStartTransferDeprecated->f_GetFunctor());

					m_fStartTransferDeprecated->f_Clear();
				}
			}
		}
	}

	template <typename tf_CStream>
	void CVersionManager::CStartDownloadVersion::CResult::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % fg_Move(m_Subscription);
		_Stream % m_VersionInfo;
		if (_Stream.f_GetVersion() >= EProtocolVersion_AsyncGeneratorFileTransfer)
		{
			if constexpr (tf_CStream::mc_Direction == NStream::EStreamDirection_Consume)
				m_FilesGenerator = fg_Construct();

			_Stream % fg_Move(*m_FilesGenerator);
		}
	}

	template <typename tf_CStream>
	void CVersionManager::CStartDownloadVersion::f_Stream(tf_CStream &_Stream)
	{
		DMibFastCheck(fs_IsValidProtocolVersion(_Stream.f_GetVersion()));
		_Stream % m_Application;
		_Stream % m_VersionIDAndPlatform;
		if (_Stream.f_GetVersion() < EProtocolVersion_AsyncGeneratorFileTransfer)
		{
			if constexpr (tf_CStream::mc_Direction == NStream::EStreamDirection_Consume)
				m_TransferContextDeprecated = fg_Construct();

			_Stream % fg_Move(*m_TransferContextDeprecated);
		}
		if (_Stream.f_GetVersion() >= EProtocolVersion_RefactorToActorFunctorsUploadDownload)
			_Stream % fg_Move(m_Subscription);
	}

	// CNewVersionNotification

	template <typename tf_CStream>
	void CVersionManager::CNewVersionNotifications::CResult::f_Stream(tf_CStream &_Stream)
	{
	}

	template <typename tf_CStream>
	void CVersionManager::CNewVersionNotification::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Application;
		_Stream % m_VersionIDAndPlatform;
		_Stream % m_VersionInfo;
	}

	template <typename tf_CStream>
	void CVersionManager::CNewVersionNotifications::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_bFullResend;
		_Stream % m_NewVersions;
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportOriginID)
			_Stream % m_OriginID;
	}

	// CSubscribeToUpdates

	template <typename tf_CStream>
	void CVersionManager::CSubscribeToUpdates::CResult::f_Stream(tf_CStream &_Stream)
	{
		if (_Stream.f_GetVersion() >= EProtocolVersion_RefactorToActorFunctorsSubscribeChanges)
			_Stream % fg_Move(m_Subscription);
		else
		{
			if constexpr (tf_CStream::mc_bConsume)
			{
				NConcurrency::CActorSubscription Subscription;
				_Stream % Subscription;
				m_Subscription = fg_Move(Subscription);
			}
			else
				_Stream % fg_Move(m_Subscription).f_GetSubscription();
		}
	}

	template <typename tf_CStream>
	void CVersionManager::CSubscribeToUpdates::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Application;

		if (_Stream.f_GetVersion() >= EProtocolVersion_RenamePlatforms)
			_Stream % m_Platforms;
		else
		{
			if constexpr (tf_CStream::mc_Direction == NStream::EStreamDirection_Consume)
			{
				NContainer::TCSet<NStr::CStr> Platforms;
				_Stream >> Platforms;
				for (auto &Platform : Platforms)
				{
					m_Platforms[Platform];
					m_Platforms[CVersionIDAndPlatform::fs_ConvertFromOldPlatform(Platform)];
				}
			}
			else
				_Stream << m_Platforms;
		}

		if (_Stream.f_GetVersion() >= EProtocolVersion_RefactorToActorFunctorsSubscribeChanges)
			_Stream % fg_Move(m_fOnNewVersions);
		else
		{
			if constexpr (tf_CStream::mc_bConsume)
			{
				NConcurrency::TCActor<> DispatchActor;
				NFunction::TCFunctionMutable<NConcurrency::TCFuture<CNewVersionNotifications::CResult> (CNewVersionNotifications &&_VersionInfo)> fOnNewVersions;

				_Stream.f_GetStream() >> DispatchActor;
				_Stream.f_GetStream() >> fOnNewVersions;

				m_fOnNewVersions = {fg_Move(DispatchActor), fg_Move(fOnNewVersions)};
			}
			else
			{
				_Stream.f_GetStream() << fg_Move(m_fOnNewVersions.f_GetActor());
				_Stream.f_GetStream() << fg_Move(m_fOnNewVersions.f_GetFunctor());
			}
		}

		_Stream % m_nInitial;
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportSubscribeToTags)
			_Stream % m_Tags;
	}

	// CChangeTags

	template <typename tf_CStream>
	void CVersionManager::CChangeTags::CResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_DeniedTags;
	}

	template <typename tf_CStream>
	void CVersionManager::CChangeTags::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Application;
		_Stream % m_VersionID;
		_Stream % m_Platform;
		_Stream % m_AddTags;
		_Stream % m_RemoveTags;
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportIncreaseRetrySequence)
			_Stream % m_bIncreaseRetrySequence;
	}
}
