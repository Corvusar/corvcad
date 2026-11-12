/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2007 Jean-Pierre Charras, jp.charras at wanadoo.fr
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

// CorvCAD headless replacement for common/confirm.cpp (KiCad 10.0.6).
// Message boxes become wx log records, which the host routes to its own log or UI.

#include <common.h>
#include <confirm.h>
#include <kiplatform/app.h>

#include <wx/intl.h>
#include <wx/log.h>


void DisplayError( wxWindow*, const wxString& aText )
{
    wxLogError( wxS( "%s" ), aText );
}


void DisplayErrorMessage( wxWindow*, const wxString& aMessage, const wxString& aExtraInfo )
{
    if( aExtraInfo.IsEmpty() )
        wxLogError( wxS( "%s" ), aMessage );
    else
        wxLogError( wxS( "%s\n%s" ), aMessage, aExtraInfo );
}


void DisplayInfoMessage( wxWindow*, const wxString& aMessage, const wxString& aExtraInfo )
{
    if( aExtraInfo.IsEmpty() )
        wxLogMessage( wxS( "%s" ), aMessage );
    else
        wxLogMessage( wxS( "%s\n%s" ), aMessage, aExtraInfo );
}


bool WarnUserIfOperatingSystemUnsupported()
{
    if( !KIPLATFORM::APP::IsOperatingSystemUnsupported() )
        return false;

    wxLogWarning( _( "This operating system is not supported by KiCad and its dependencies." ) );
    return true;
}