// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void CDirectoryManifestFile::f_Stream(tf_CStream &_Stream, uint32 _Version)
	{
		_Stream % m_Digest;
		_Stream % m_Length;
		_Stream % m_WriteTime;
		_Stream % m_SymlinkData;
		_Stream % m_Attributes;
		_Stream % m_Owner;
		_Stream % m_Group;
		_Stream % m_Flags;
	}
	
	template <typename tf_CStream>
	void CDirectoryManifest::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = EManifestStreamVersion;
		_Stream % Version;
		if (Version < 0x101 || Version > EManifestStreamVersion)
			DMibError("Invalid directory manifest version");
		DMibBinaryStreamVersion(_Stream, Version);

		if (_Stream.mc_Direction == NStream::EStreamDirection_Feed)
		{
			uint32 nFiles = m_Files.f_GetLen();
			_Stream << nFiles;
			for (auto &File : m_Files)
			{
				_Stream << m_Files.fs_GetKey(File);
				File.f_Stream(_Stream, Version);
			}
		}
		else
		{
			uint32 nFiles;
			_Stream >> nFiles;
			while (nFiles--)
			{
				NStr::CStr Key;
				_Stream >> Key;
				auto &File = m_Files[Key];
				File.f_Stream(_Stream, Version);
			}
		}
	}
}
