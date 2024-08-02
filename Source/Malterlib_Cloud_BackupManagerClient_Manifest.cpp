// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"

namespace NMib::NCloud
{
	auto CBackupManagerClient::CInternal::f_UpdateManifest(CStr const &_FileName, CStr const &_OriginalFileName, bool _bDirtyHint) -> TCFuture<CUpdateManifestResult>
	{
		if (_FileName.f_IsEmpty())
			co_return CUpdateManifestResult{};
			
		bool bDirty = _bDirtyHint;
		TCSharedPointer<CAppendFileState> pAppendState;
		
		{
			auto pFile = m_Manifest.m_Files.f_FindEqual(_FileName);
			if (pFile && (pFile->m_Flags & EDirectoryManifestSyncFlag_Append))
				pAppendState = *m_AppendStates(_FileName, fg_Construct());
			else if (!pFile && (CDirectoryManifest::fs_GetSyncFlags(m_Config.m_ManifestConfig, _FileName) & EDirectoryManifestSyncFlag_Append))
				pAppendState = *m_AppendStates(_FileName, fg_Construct());
		}
		
		auto BlockingActorCheckout = fg_BlockingActor();

		CUpdateManifestResult Result = co_await
			(
				g_Dispatch(BlockingActorCheckout) / [_FileName, _OriginalFileName, Config = m_Config, pAppendState, bDirty]() mutable -> CUpdateManifestResult
				{
					auto AbsoluteFileName = CFile::fs_AppendPath(Config.m_ManifestConfig.m_Root, _OriginalFileName);

					if (pAppendState && !bDirty && !pAppendState->m_bIsLink && pAppendState->m_bIsValid && pAppendState->m_File.f_IsValid())
					{
						auto &AppendState = *pAppendState;

						CUniqueFileIdentifier FileID;
						try
						{
							FileID = CFile::fs_GetUniqueIdentifierOnLink(AbsoluteFileName);
						}
						catch (NFile::CExceptionFile const &)
						{
							// Detect deletes
						}

						if (FileID == AppendState.m_FileID) // Detect renames
						{
							AppendState.m_ManifestFile.m_Length = AppendState.m_File.f_GetLength();

							CFileIoTempBuffer Buffer;

							while (AppendState.m_ChecksumState.m_Position < AppendState.m_ManifestFile.m_Length)
							{
								auto BufferResult = Buffer.f_UseBuffer(AppendState.m_ManifestFile.m_Length - AppendState.m_File.f_GetPosition());
								AppendState.m_File.f_Read(BufferResult.m_pBuffer, BufferResult.m_nBytes);
								AppendState.m_ChecksumState.m_DigestState.f_AddData(BufferResult.m_pBuffer, BufferResult.m_nBytes);
								AppendState.m_ChecksumState.m_Position = uint64(AppendState.m_File.f_GetPosition());
							}

							AppendState.m_ManifestFile.m_WriteTime = AppendState.m_File.f_GetWriteTime();
							AppendState.m_ManifestFile.m_Digest = AppendState.m_ChecksumState.m_DigestState;

							CUpdateManifestResult Result;
							Result.m_bExists = true;
							Result.m_Appended = true;
							Result.m_bChecksumValid = true;
							Result.m_ManifestFile = AppendState.m_ManifestFile;
							Result.m_FileID = AppendState.m_FileID;
							Result.m_ChecksumState = AppendState.m_ChecksumState;

							return Result;
						}
					}

					auto fTryUpdate = [&]() -> CUpdateManifestResult
						{
							if (!CFile::fs_FileExists(AbsoluteFileName, EFileAttrib_File | EFileAttrib_Link | EFileAttrib_Directory))
							{
								if (pAppendState)
									pAppendState->m_File.f_Close();
								return CUpdateManifestResult{.m_bExists = false};
							}

							CDirectoryManifestFile ManifestFile;
							ManifestFile.m_Attributes = CFile::fs_GetAttributes(AbsoluteFileName);

							CFile::CFileChecksumState_SHA256 ChecksumState;

							CDirectoryManifest::fs_UpdateManifestFile(Config.m_ManifestConfig, _FileName, ManifestFile, _OriginalFileName, &ChecksumState, gc_ChecksumFileFlags);

							bool bIsLink = false;
							CUniqueFileIdentifier FileID;
							if (ManifestFile.m_Attributes & EFileAttrib_Link)
								bIsLink = true;

							FileID = CFile::fs_GetUniqueIdentifierOnLink(AbsoluteFileName);

							CUpdateManifestResult Result = {.m_ManifestFile = ManifestFile, .m_FileID = FileID, .m_bExists = true};

							if (pAppendState)
							{
								auto &AppendState = *pAppendState;
								AppendState.m_bIsLink = bIsLink;
								AppendState.m_File.f_Close();
								AppendState.m_FileID = FileID;
								if (!bIsLink)
								{
									AppendState.m_File = fg_Move(*ChecksumState.m_pFile);
									AppendState.m_ManifestFile = ManifestFile;
									AppendState.m_ChecksumState.m_DigestState = ChecksumState.m_Hash;
									AppendState.m_ChecksumState.m_Position = uint64(AppendState.m_File.f_GetPosition());
									AppendState.m_bIsValid = true;
									Result.m_bChecksumValid = true;
									Result.m_ChecksumState = AppendState.m_ChecksumState;
								}
							}

							CStr Directory = CFile::fs_GetPath(_FileName);
							while (!Directory.f_IsEmpty())
							{
								auto &UpdatedDirectory = Result.m_UpdatedDirectories[Directory];

								CStr OriginalFileName;
								if (_FileName != _OriginalFileName)
								{
									if (ManifestFile.f_IsDirectory())
										OriginalFileName = _OriginalFileName;
									else
										OriginalFileName = CFile::fs_GetPath(_OriginalFileName);
								}
								else
									OriginalFileName = Directory;

								UpdatedDirectory.m_ManifestFile.m_Attributes = CFile::fs_GetAttributes(CFile::fs_AppendPath(Config.m_ManifestConfig.m_Root, OriginalFileName));

								CDirectoryManifest::fs_UpdateManifestFile(Config.m_ManifestConfig, Directory, UpdatedDirectory.m_ManifestFile, OriginalFileName, nullptr, gc_ChecksumFileFlags);
								Directory = CFile::fs_GetPath(Directory);
							}

							return Result;
						}
					;

					auto LastException = DMibImpErrorInstance(NFile::CExceptionFile, "DUMMY");
					mint nRetries = 0;
					while (true)
					{
						try
						{
							return fTryUpdate();
						}
						catch (NFile::CExceptionFile const &_Exception)
						{
							if (LastException.f_GetErrorStr() == _Exception.f_GetErrorStr())
							{
								if (++nRetries == 3)
									throw;
							}
							else
								nRetries = 0;
							LastException = _Exception;
						}
					}
				}
			)
		;

		if (Result.m_bChecksumValid)
		{
			m_ChecksumState[_FileName] = Result.m_ChecksumState;
		}
		else
			m_ChecksumState.f_Remove(_FileName);

		if (!Result.m_bExists)
		{
			m_AppendStates.f_Remove(_FileName);

			if (m_Manifest.m_Files.f_Remove(_FileName))
				Result.m_bRemoved = true;
			if (m_ManifestFileIDs.f_Remove(_FileName))
				Result.m_bIDChanged = true;
		}
		else
		{
			auto Mapped = m_Manifest.m_Files(_FileName);
			if (Mapped.f_WasCreated())
				Result.m_bAdded = true;

			if (Result.m_ManifestFile.f_IsFile())
			{
				auto &FileID = m_ManifestFileIDs[_FileName];
				if (Result.m_FileID != FileID)
				{
					FileID = Result.m_FileID;
					Result.m_bIDChanged = true;
				}
			}
			else
			{
				if (m_ManifestFileIDs.f_Remove(_FileName))
					Result.m_bIDChanged = true;
			}

			*Mapped = Result.m_ManifestFile;

			for (auto iDirectory = Result.m_UpdatedDirectories.f_GetIterator(); iDirectory;)
			{
				auto &Directory = *iDirectory;
				auto Mapped = m_Manifest.m_Files(iDirectory.f_GetKey());
				auto &ManifestFile = *Mapped;

				if (Mapped.f_WasCreated())
				{
					ManifestFile = Directory.m_ManifestFile;
					Directory.m_bAdded = true;
					++iDirectory;
					continue;
				}

				if (ManifestFile != Directory.m_ManifestFile)
				{
					ManifestFile = Directory.m_ManifestFile;
					++iDirectory;
					continue;
				}

				iDirectory.f_Remove();
			}
		}

		co_return fg_Move(Result);
	}
}
