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

// CorvCAD iOS stand-in for KiCad 10.0.6 eeschema/sch_base_frame.h; see eda_base_frame.h.

#ifndef SCH_BASE_FRAME_H_
#define SCH_BASE_FRAME_H_

#include <cstddef>
#include <future>
#include <utility>
#include <vector>

#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/string.h>

#include <eda_draw_frame.h>
#include <frame_type.h>
#include <libraries/library_table.h>
#include <sch_draw_panel.h>
#include <sch_screen.h>
#include <schematic_holder.h>
#include <schematic_settings.h>
#include <template_fieldnames.h>

class SCH_BASE_FRAME : public EDA_DRAW_FRAME, public SCHEMATIC_HOLDER
{
public:
    virtual void HypertextCommand( const wxString& aHref ) {}
};

#endif  // SCH_BASE_FRAME_H_