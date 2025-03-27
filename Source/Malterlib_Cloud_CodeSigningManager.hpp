// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void CCodeSigningManager::CDownloadFileContents::f_Stream(tf_CStream &_Stream)
	{
		_Stream % fg_Move(m_DataGenerator);
		_Stream % fg_Move(m_Subscription);
		_Stream % m_StartPosition;
	}

	template <typename tf_CStream>
	void CCodeSigningManager::CDownloadFile::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_FilePath;
		_Stream % m_FileAttributes;
		_Stream % m_WriteTime;
		_Stream % m_FileSize;
		_Stream % m_SymlinkContents;
		_Stream % fg_Move(m_fGetDataGenerator);
	}

	template <typename tf_CStream>
	void CCodeSigningManager::CSignFiles::CResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % fg_Move(m_fGetSignature);
	}

	template <typename tf_CStream>
	void CCodeSigningManager::CSignFiles::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Authority;
		_Stream % m_SigningCert;
		_Stream % m_QueueSize;
		_Stream % fg_Move(m_FilesGenerator);
	}
}
