/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// CorvCAD headless replacement for common/widgets/ui_common.cpp (KiCad 10.0.6).
// Provides only what model code calls. There is no window to measure, so status text is
// shortened like menu text.

#include <string_utils.h>
#include <widgets/ui_common.h>
#include <widgets/report_severity.h>


SEVERITY SeverityFromString( const wxString& aSeverity )
{
    if( aSeverity == wxT( "warning" ) )
        return RPT_SEVERITY_WARNING;
    else if( aSeverity == wxT( "ignore" ) )
        return RPT_SEVERITY_IGNORE;
    else
        return RPT_SEVERITY_ERROR;
}


wxString SeverityToString( const SEVERITY& aSeverity )
{
    if( aSeverity == RPT_SEVERITY_IGNORE )
        return wxT( "ignore" );
    else if( aSeverity == RPT_SEVERITY_WARNING )
        return wxT( "warning" );
    else
        return wxT( "error" );
}


wxString KIUI::EllipsizeMenuText( const wxString& aString )
{
    wxString msg = UnescapeString( aString );

    msg.Replace( wxT( "\n" ), wxT( " " ) );
    msg.Replace( wxT( "\r" ), wxT( " " ) );
    msg.Replace( wxT( "\t" ), wxT( " " ) );

    if( msg.Length() > 36 )
        msg = msg.Left( 34 ) + wxT( "..." );

    return msg;
}


wxString KIUI::EllipsizeStatusText( wxWindow*, const wxString& aString )
{
    return EllipsizeMenuText( aString );
}
