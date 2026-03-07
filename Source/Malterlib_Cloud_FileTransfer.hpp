// Copyright © 2025 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CTypeTo, typename tf_CTypeFrom>
	NConcurrency::TCAsyncGenerator<tf_CTypeTo> CFileTransferSendDownloadFile::fs_TranslateGenerator(NConcurrency::TCAsyncGenerator<tf_CTypeFrom> _FilesGenerator)
	{
		NStorage::TCSharedPointer<typename NConcurrency::TCAsyncGenerator<tf_CTypeFrom>::CPipelinedIterator> pIterator
			= fg_Construct(co_await fg_Move(_FilesGenerator).f_GetPipelinedIterator())
		;

		for (auto &iGenerator = *pIterator; iGenerator; co_await ++iGenerator)
		{
			auto Subscription = fg_Move((*iGenerator).m_fGetDataGenerator.f_GetSubscription());
			co_yield tf_CTypeTo
				{
					.m_FilePath = fg_Move((*iGenerator).m_FilePath)
					, .m_FileAttributes = fg_Move((*iGenerator).m_FileAttributes)
					, .m_WriteTime = fg_Move((*iGenerator).m_WriteTime)
					, .m_FileSize = (*iGenerator).m_FileSize
					, .m_SymlinkContents = fg_Move((*iGenerator).m_SymlinkContents)
					, .m_fGetDataGenerator = NConcurrency::g_ActorFunctor(fg_Move(Subscription)) /
					[
						fGetDataGenerator = fg_Move((*iGenerator).m_fGetDataGenerator)
						, pIterator // Keep iterator alive so the original subscription doesn't go out of scope too early
						, AllowDestroy = NConcurrency::g_AllowWrongThreadDestroy
					]
					(uint64 _StartPosition, NCryptography::CHashDigest_SHA256 _StartDigest) -> NConcurrency::TCFuture<typename tf_CTypeTo::CDownloadFileContents>
					{
						auto Return = co_await fGetDataGenerator(_StartPosition, _StartDigest);
						co_return typename tf_CTypeTo::CDownloadFileContents
							{
								.m_DataGenerator = fg_Move(Return.m_DataGenerator)
								, .m_Subscription = NConcurrency::g_ActorSubscription / [Subscription0 = fg_Move(Return.m_Subscription), pIterator]() -> NConcurrency::TCFuture<void>
								{
									if (Subscription0)
										co_await Subscription0->f_Destroy();

									co_return {};
								}
								, .m_StartPosition = fg_Move(Return.m_StartPosition)
							}
						;
					}
				}
			;
		}

		co_return {};
	}
}
