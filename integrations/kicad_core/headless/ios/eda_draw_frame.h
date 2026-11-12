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

// CorvCAD iOS stand-in for KiCad 10.0.6 include/eda_draw_frame.h; see eda_base_frame.h.

#ifndef DRAW_FRAME_H_
#define DRAW_FRAME_H_

#include <class_draw_panel_gal.h>
#include <gal/color4d.h>
#include <gal/gal_display_options.h>
#include <gal_display_options_common.h>
#include <kiid.h>
#include <kiway_player.h>
#include <lib_id.h>
#include <widgets/msgpanel.h>

class EDA_ITEM;

// Same values as upstream: model code compares a frame's GetName() with them.
#define LIB_EDIT_FRAME_NAME           wxT( "LibeditFrame" )
#define LIB_VIEW_FRAME_NAME           wxT( "ViewlibFrame" )
#define SCH_EDIT_FRAME_NAME           wxT( "SchematicFrame" )
#define SYMBOL_CHOOSER_FRAME_NAME     wxT( "SymbolChooserFrame" )
#define PL_EDITOR_FRAME_NAME          wxT( "PlEditorFrame" )
#define FOOTPRINT_WIZARD_FRAME_NAME   wxT( "FootprintWizard" )
#define FOOTPRINT_CHOOSER_FRAME_NAME  wxT( "FootprintChooserFrame" )
#define FOOTPRINT_EDIT_FRAME_NAME     wxT( "ModEditFrame" )
#define FOOTPRINT_VIEWER_FRAME_NAME   wxT( "ModViewFrame" )
#define PCB_EDIT_FRAME_NAME           wxT( "PcbFrame" )

class EDA_DRAW_FRAME : public KIWAY_PLAYER
{
public:
    virtual EDA_ITEM* ResolveItem( const KIID& aId, bool aAllowNullptrReturn = false ) const
    {
        return nullptr;
    }
};

#endif  // DRAW_FRAME_H_