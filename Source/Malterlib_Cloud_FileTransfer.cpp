// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_FileTransfer.h"
#include "Malterlib_Cloud_FileTransfer_Internal.h"

namespace NMib::NCloud
{
	using namespace NConcurrency;

	// CFileTransferContextDeprecated
	CFileTransferContextDeprecated::CFileTransferContextDeprecated()
		: mp_pInternal(fg_Construct())
	{
	}

	CFileTransferContextDeprecated::~CFileTransferContextDeprecated() = default;
	CFileTransferContextDeprecated::CFileTransferContextDeprecated(CFileTransferContextDeprecated &&_Other) = default;
	CFileTransferContextDeprecated &CFileTransferContextDeprecated::operator =(CFileTransferContextDeprecated &&_Other) = default;


	void CFileTransferContextDeprecated::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		mp_pInternal->f_Feed(_Stream);
	}

	void CFileTransferContextDeprecated::f_Consume(CDistributedActorReadStream &_Stream)
	{
		mp_pInternal->f_Consume(_Stream);
	}

	void CFileTransferContextDeprecated::CInternal::f_Feed(CDistributedActorWriteStream &_Stream)
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
		_Stream << m_Manifest;
		_Stream << m_QueueSize;
		_Stream << fg_Move(m_DispatchActor);
		_Stream << fg_Move(m_fSendPart);
		_Stream << fg_Move(m_fStateChange);
		// Any version management needs to be additions past this point
	}

	void CFileTransferContextDeprecated::CInternal::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Version;
		m_Version = fg_Min(m_Version, EProtocolVersion_Current);
		if (m_Version < 0x101)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
		_Stream >> m_Manifest;
		_Stream >> m_QueueSize;
		_Stream >> m_DispatchActor;
		_Stream >> m_fSendPart;
		_Stream >> m_fStateChange;
	}


	void CFileTransferContextDeprecated::CInternal::CFileInfo::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_FileSize;
	}

	void CFileTransferContextDeprecated::CInternal::CFileInfo::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_FileSize;
	}

	NStr::CStr const &CFileTransferContextDeprecated::CInternal::CFileInfo::f_GetPath() const
	{
		return NContainer::TCMap<NStr::CStr, CFileInfo>::fs_GetKey(this);
	}

	void CFileTransferContextDeprecated::CInternal::CManifest::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_Files;
	}

	void CFileTransferContextDeprecated::CInternal::CManifest::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Files;
	}


	// CSendPart

	CFileTransferContextDeprecated::CInternal::CSendPart::CSendPart(uint32 _Version)
		: m_Version(_Version)
	{
	}

	void CFileTransferContextDeprecated::CInternal::CSendPart::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
	}

	void CFileTransferContextDeprecated::CInternal::CSendPart::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version < 0x101 || m_Version > EProtocolVersion_Current)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
	}

	auto CFileTransferContextDeprecated::CInternal::CSendPart::f_GetResult() const -> CResult
	{
		CResult Result;
		Result.m_Version = m_Version;
		return Result;
	}

	void CFileTransferContextDeprecated::CInternal::CSendPart::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
		_Stream << m_FilePath;
		_Stream << m_FilePosition;
		_Stream << m_Data;
		_Stream << m_bFinished;
		if (m_bFinished)
		{
			_Stream << m_FileAttributes;
			_Stream << m_WriteTime;
		}
	}

	void CFileTransferContextDeprecated::CInternal::CSendPart::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version < 0x101 || m_Version > EProtocolVersion_Current)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
		_Stream >> m_FilePath;
		_Stream >> m_FilePosition;
		_Stream >> m_Data;
		_Stream >> m_bFinished;
		if (m_bFinished)
		{
			_Stream >> m_FileAttributes;
			_Stream >> m_WriteTime;
		}
	}

	// CTransferResult

	fp64 CFileTransferResult::f_BytesPerSecond() const
	{
		return fp64(m_nBytes) / m_nSeconds;
	}

	void CFileTransferResult::f_Feed(NConcurrency::CDistributedActorWriteStream &_Stream) const
	{
		_Stream << m_nBytes;
		_Stream << m_nSeconds;
	}

	void CFileTransferResult::f_Consume(NConcurrency::CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_nBytes;
		_Stream >> m_nSeconds;
	}

	// CStateChange

	CFileTransferContextDeprecated::CInternal::CStateChange::CStateChange(uint32 _Version)
		: m_Version(_Version)
	{
	}

	void CFileTransferContextDeprecated::CInternal::CStateChange::CResult::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
	}

	void CFileTransferContextDeprecated::CInternal::CStateChange::CResult::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version < 0x101 || m_Version > EProtocolVersion_Current)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
	}

	auto CFileTransferContextDeprecated::CInternal::CStateChange::f_GetResult() const -> CResult
	{
		CResult Result;
		Result.m_Version = m_Version;
		return Result;
	}

	void CFileTransferContextDeprecated::CInternal::CStateChange::f_Feed(CDistributedActorWriteStream &_Stream) const
	{
		DMibRequire(m_Version != 0);
		_Stream << m_Version;
		_Stream << m_State;
		if (m_State == EState_Error)
			_Stream << m_Error;
		else if (m_State == EState_Finished)
			_Stream << m_Finished;
	}

	void CFileTransferContextDeprecated::CInternal::CStateChange::f_Consume(CDistributedActorReadStream &_Stream)
	{
		_Stream >> m_Version;
		if (m_Version < 0x101 || m_Version > EProtocolVersion_Current)
			DMibError("Invalid protocol version");
		DMibBinaryStreamVersion(_Stream, m_Version);
		_Stream >> m_State;
		if (m_State == EState_Error)
			_Stream >> m_Error;
		else if (m_State == EState_Finished)
			_Stream >> m_Finished;
	}
}
