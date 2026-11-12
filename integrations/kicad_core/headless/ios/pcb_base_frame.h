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
// CorvCAD iOS stand-in for KiCad 10.0.6 include/pcb_base_frame.h; see eda_base_frame.h.

#ifndef PCB_BASE_FRAME_H
#define PCB_BASE_FRAME_H

#include <vector>

#include <wx/datetime.h>
#include <wx/filename.h>
#include <wx/timer.h>

#include <eda_draw_frame.h>
#include <eda_units.h>
#include <lib_id.h>
#include <pcb_display_options.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_origin_transforms.h>
#include <pcb_screen.h>

class BOARD;

class PCB_BASE_FRAME : public EDA_DRAW_FRAME
{
};

#endif  // PCB_BASE_FRAME_H
