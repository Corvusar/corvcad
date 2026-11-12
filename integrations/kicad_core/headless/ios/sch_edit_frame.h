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

// CorvCAD iOS stand-in for KiCad 10.0.6 eeschema/sch_edit_frame.h; see eda_base_frame.h.

#ifndef  SCH_EDIT_FRAME_H
#define  SCH_EDIT_FRAME_H

#include <wx/filename.h>
#include <wx/utils.h>

#include <core/typeinfo.h>
#include <eeschema_settings.h>
#include <math/box2.h>
#include <sch_base_frame.h>
#include <template_fieldnames.h>

class SCH_EDIT_FRAME : public SCH_BASE_FRAME
{
};

#endif  // SCH_EDIT_FRAME_H