// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/CommandLine/TableRenderer>

#include "Malterlib_Cloud_App_CodeSigningManager.h"

namespace NMib::NCloud::NCodeSigningManager
{
	void CCodeSigningManagerActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Code Signing Manager"
				, "Manages code signing."
			)
		;

		auto AuthorityManagement = o_CommandLine.f_AddSection("Authority Management", "Commands to manage authorities");

		auto SettingsOption_EllipticCurveType = "EllipticCurveType?"_o=
			{
				"Names"_o= _o["--elliptic-curve-type"]
				, "Default"_o= "secp521r1"
				, "Type"_o= COneOf{"secp256r1", "secp384r1", "secp521r1", "X25519"}
				, "Description"_o= "The type of elliptic curve to use for the EC certificate."
			}
		;

		auto SettingsOption_RSASize = "RSASize?"_o=
			{
				"Names"_o= _o["--rsa-size"]
				, "Type"_o= 4096
				, "Description"_o= "The size of the RSA certificate."
			}
		;

		auto SettingsOption_Authority = "Authority?"_o=
			{
				"Names"_o= _o["--authority"]
				, "Default"_o= ""
				, "Type"_o= ""
				, "Description"_o= "The certificate authority to use"
			}
		;

		auto fStripDefault = [](auto &&_Template)
			{
				auto Return = _Template;
				Return.m_Value.f_RemoveMember("Default");
				return Return;
			}
		;
		auto fStripOptional = [](auto &&_Template)
			{
				auto Return = _Template;
				Return.m_Key = Return.m_Key.f_Replace("?", "");
				return Return;
			}
		;

		AuthorityManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--authority-create"]
					, "Description"_o= "Create a certificate authority\n"
					, "Options"_o=
					{
						"Name"_o=
						{
							"Names"_o= _o["--name"]
							, "Type"_o= ""
							, "Description"_o= "Name of the certificate authority"
						}
						, SettingsOption_EllipticCurveType
						, SettingsOption_RSASize
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_AuthorityCreate(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
		AuthorityManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--authority-list"]
					, "Description"_o= "List certificate authorities."
					, "Options"_o=
					{
						"Verbose?"_o=
						{
							"Names"_o= _o["--verbose", "-v"]
							, "Default"_o= false
							, "Description"_o= "Display more extensive information about the ca."
						}
						, SettingsOption_Authority
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_AuthorityList(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
		AuthorityManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--authority-resync"]
					, "Description"_o= "Update certificate authorities on out of date secret managers."
					, "Options"_o=
					{
						SettingsOption_Authority
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_AuthorityResync(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
		AuthorityManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--authority-info"]
					, "Description"_o= "Get information about a certificate authority as JSON."
					, "Options"_o=
					{
						"Name"_o=
						{
							"Names"_o= _o["--name"]
							, "Type"_o= ""
							, "Description"_o= "Name of the certificate authority"
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_AuthorityInfo(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		auto SigningCertManagement = o_CommandLine.f_AddSection("Signing Cert Management", "Commands to manage signing certificates");

		auto SettingsOption_SigningCert = "SigningCert?"_o=
			{
				"Names"_o= _o["--signing-cert"]
				, "Default"_o= ""
				, "Type"_o= ""
				, "Description"_o= "Name of the signing cert"
			}
		;

		SigningCertManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--signing-cert-create"]
					, "Description"_o= "Create a signing cert\n"
					, "Options"_o=
					{
						fStripOptional(fStripDefault(SettingsOption_Authority))
						, fStripOptional(fStripDefault(SettingsOption_SigningCert))
						, SettingsOption_EllipticCurveType
						, SettingsOption_RSASize
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SigningCertCreate(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
		SigningCertManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--signing-cert-list"]
					, "Description"_o= "List signing certificates."
					, "Options"_o=
					{
						"Verbose?"_o=
						{
							"Names"_o= _o["--verbose", "-v"]
							, "Default"_o= false
							, "Description"_o= "Display more extensive information about the ca."
						}
						, SettingsOption_Authority
						, SettingsOption_SigningCert
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SigningCertList(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
		SigningCertManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--signing-cert-resync"]
					, "Description"_o= "Update signing certificates on out of date secret managers."
					, "Options"_o=
					{
						SettingsOption_Authority
						, SettingsOption_SigningCert
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SigningCertResync(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
		SigningCertManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--signing-cert-reissue-certificate"]
					, "Description"_o= "Reissue certificates that are about to expire."
					, "Options"_o=
					{
						"Days?"_o=
						{
							"Names"_o= _o["--days"]
							, "Default"_o= 365
							, "Description"_o= "Reissue certificates that are about to expire within these number of days."
						}
						, SettingsOption_Authority
						, SettingsOption_SigningCert
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_SigningCertReissue(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
	}
}
