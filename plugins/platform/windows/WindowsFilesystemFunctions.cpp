/*
 * WindowsFilesystemFunctions.cpp - implementation of WindowsFilesystemFunctions class
 *
 * Copyright (c) 2017-2018 Tobias Junghans <tobydox@users.sf.net>
 *
 * This file is part of Veyon - http://veyon.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include <QDir>

#include <shlobj.h>
#include <accctrl.h>
#include <aclapi.h>

#include "WindowsCoreFunctions.h"
#include "WindowsFilesystemFunctions.h"


static QString windowsConfigPath( REFKNOWNFOLDERID folderId )
{
	QString result;

	wchar_t* path = nullptr;
	if( SHGetKnownFolderPath( folderId, KF_FLAG_DEFAULT, nullptr, &path ) == S_OK )
	{
		result = QString::fromWCharArray( path );
		CoTaskMemFree( path );
	}

	return result;
}



QString WindowsFilesystemFunctions::personalAppDataPath() const
{
	return windowsConfigPath( FOLDERID_RoamingAppData ) + QDir::separator() + QStringLiteral("Veyon") + QDir::separator();
}



QString WindowsFilesystemFunctions::globalAppDataPath() const
{
	return windowsConfigPath( FOLDERID_ProgramData ) + QDir::separator() + QStringLiteral("Veyon") + QDir::separator();
}



QString WindowsFilesystemFunctions::fileOwnerGroup( const QString& filePath )
{
	PSID ownerSID = nullptr;
	PSECURITY_DESCRIPTOR securityDescriptor = nullptr;

	const auto secInfoResult = GetNamedSecurityInfo( (LPWSTR) filePath.utf16(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
													 &ownerSID, nullptr, nullptr, nullptr, &securityDescriptor );
	if( secInfoResult != ERROR_SUCCESS )
	{
		qCritical() << Q_FUNC_INFO << "GetSecurityInfo() failed:" << secInfoResult;
		return QString();
	}

	wchar_t name[PATH_MAX];
	DWORD nameSize = 0;
	DWORD domainLen = 0;
	SID_NAME_USE sidNameUse;

	const auto lookupSidResult = LookupAccountSid( nullptr, ownerSID, name, &nameSize,
												   nullptr, &domainLen, &sidNameUse );
	if( lookupSidResult != ERROR_SUCCESS )
	{
		qCritical() << Q_FUNC_INFO << "LookupAccountSid() failed:" << lookupSidResult;
		return QString();
	}

	return QString::fromWCharArray( name );
}



bool WindowsFilesystemFunctions::setFileOwnerGroup( const QString& filePath, const QString& ownerGroup )
{
	DWORD sidLen = SECURITY_MAX_SID_SIZE;
	char ownerGroupSID[SECURITY_MAX_SID_SIZE];
	wchar_t domainName[MAX_PATH];
	domainName[0] = 0;
	DWORD domainLen = MAX_PATH;
	SID_NAME_USE sidNameUse;

	if( LookupAccountName( nullptr, (LPCWSTR) ownerGroup.utf16(), ownerGroupSID, &sidLen,
						   domainName, &domainLen, &sidNameUse ) == false )
	{
		qCritical( "Could not look up SID structure" );
		return false;
	}

	WindowsCoreFunctions::enablePrivilege( SE_TAKE_OWNERSHIP_NAME, true );

	const auto result = SetNamedSecurityInfo( (LPWSTR) filePath.utf16(), SE_FILE_OBJECT,
											  OWNER_SECURITY_INFORMATION, ownerGroupSID, nullptr, nullptr, nullptr );

	WindowsCoreFunctions::enablePrivilege( SE_TAKE_OWNERSHIP_NAME, false );

	if( result != ERROR_SUCCESS )
	{
		qCritical() << Q_FUNC_INFO << "SetNamedSecurityInfo() failed:" << result;
	}

	return result == ERROR_SUCCESS;
}



bool WindowsFilesystemFunctions::setFileOwnerGroupPermissions( const QString& filePath, QFile::Permissions permissions )
{
	PSID ownerSID = nullptr;
	PSECURITY_DESCRIPTOR securityDescriptor = nullptr;

	const auto secInfoResult = GetNamedSecurityInfo( (LPWSTR) filePath.utf16(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
													 &ownerSID, nullptr, nullptr, nullptr, &securityDescriptor );
	if( secInfoResult != ERROR_SUCCESS )
	{
		qCritical() << Q_FUNC_INFO << "GetSecurityInfo() failed:" << secInfoResult;
		return false;
	}

	PSID adminSID = nullptr;
	SID_IDENTIFIER_AUTHORITY SIDAuthNT = SECURITY_NT_AUTHORITY;
	if( AllocateAndInitializeSid( &SIDAuthNT, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
								  0, 0, 0, 0, 0, 0, &adminSID ) == false )
	{
		return false;
	}

	const int NUM_ACES = 2;
	EXPLICIT_ACCESS ea[NUM_ACES];

	ZeroMemory( &ea, NUM_ACES * sizeof(EXPLICIT_ACCESS) );

	// set read access for owner
	ea[0].grfAccessPermissions = 0;
	if( permissions & QFile::ReadGroup )
	{
		ea[0].grfAccessPermissions |= GENERIC_READ;
	}
	if( permissions & QFile::WriteGroup )
	{
		ea[0].grfAccessPermissions |= GENERIC_WRITE;
	}
	if( permissions & QFile::ExeGroup )
	{
		ea[0].grfAccessPermissions |= GENERIC_EXECUTE;
	}
	ea[0].grfAccessMode = SET_ACCESS;
	ea[0].grfInheritance = NO_INHERITANCE;
	ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea[0].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
	ea[0].Trustee.ptstrName = (LPTSTR) ownerSID;

	// set full control for Administrators
	ea[1].grfAccessPermissions = GENERIC_ALL;
	ea[1].grfAccessMode = SET_ACCESS;
	ea[1].grfInheritance = NO_INHERITANCE;
	ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
	ea[1].Trustee.ptstrName = (LPTSTR) adminSID;

	PACL acl = nullptr;
	if( SetEntriesInAcl( NUM_ACES, ea, nullptr, &acl ) != ERROR_SUCCESS )
	{
		qCritical() << Q_FUNC_INFO << "SetEntriesInAcl() failed";
		FreeSid( adminSID );
		return false;
	}

	const auto result = SetNamedSecurityInfo( (LPWSTR) filePath.utf16(), SE_FILE_OBJECT,
											  DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr );

	if( result != ERROR_ACCESS_DENIED )
	{
		qCritical() << Q_FUNC_INFO << "SetNamedSecurityInfo() failed:" << result;
	}

	FreeSid( adminSID );
	LocalFree( acl );

	return result == ERROR_SUCCESS;
}
