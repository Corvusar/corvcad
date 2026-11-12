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
// CorvCAD iOS stand-in for KiCad 10.0.6 include/widgets/kistatusbar.h. wxWidgets' iOS port has
// no wxStatusBar. PGM_BASE forwards library load messages to registered status bars, and
// headless none register; LOAD_MESSAGE must match upstream.

#ifndef KISTATUSBAR_H
#define KISTATUSBAR_H

#include <kicommon.h>
#include <widgets/report_severity.h>

#include <wx/string.h>

#include <vector>

struct LOAD_MESSAGE
{
    wxString  message;
    SEVERITY  severity;
};

class KICOMMON_API KISTATUSBAR
{
public:
    void ClearWarningMessages( const wxString& aSource = wxEmptyString );
    void AddWarningMessages( const wxString& aSource, const std::vector<LOAD_MESSAGE>& aMessages );
};

#endif  // KISTATUSBAR_H