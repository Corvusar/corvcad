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

// CorvCAD iOS stand-in for KiCad 10.0.6 eeschema/tools/sch_navigate_tool.h. The tool itself is
// editor code; SCH_FIELD only reads the hypertext back-link constant, which must match.

#ifndef SCH_NAVIGATE_TOOL_H
#define SCH_NAVIGATE_TOOL_H

#include <wx/string.h>

class SCH_NAVIGATE_TOOL
{
public:
    static inline wxString g_BackLink = wxT( "HYPERTEXT_BACK" );
};

#endif  // SCH_NAVIGATE_TOOL_H