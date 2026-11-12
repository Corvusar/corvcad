/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Alex Shvartzkop <dudesuchamazing@gmail.com>
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

// CorvCAD headless replacement for pcbnew/generators/pcb_tuning_pattern_edit.cpp (KiCad 10.0.6
// with patch 0007). The editor half of PCB_TUNING_PATTERN re-tunes through the push-and-shove
// router, previews and dialogs. A headless board only loads, saves and checks the pattern's
// existing copper, so these virtuals do nothing and the pattern keeps its stored members.
// EditStart is the class's key function, so its vtable is emitted here.

#include <pcb_generator.h>
#include <board.h>
#include <pcb_track.h>
#include <generators/pcb_tuning_pattern.h>


void PCB_TUNING_PATTERN::EditStart( GENERATOR_TOOL*, BOARD*, BOARD_COMMIT* )
{
}


bool PCB_TUNING_PATTERN::Update( GENERATOR_TOOL*, BOARD*, BOARD_COMMIT* )
{
    return false;
}


void PCB_TUNING_PATTERN::EditFinish( GENERATOR_TOOL*, BOARD*, BOARD_COMMIT* )
{
}


void PCB_TUNING_PATTERN::EditCancel( GENERATOR_TOOL*, BOARD*, BOARD_COMMIT* )
{
}


void PCB_TUNING_PATTERN::Remove( GENERATOR_TOOL*, BOARD*, BOARD_COMMIT* )
{
}


bool PCB_TUNING_PATTERN::MakeEditPoints( EDIT_POINTS& ) const
{
    return false;
}


bool PCB_TUNING_PATTERN::UpdateFromEditPoints( EDIT_POINTS& )
{
    return false;
}


bool PCB_TUNING_PATTERN::UpdateEditPoints( EDIT_POINTS& )
{
    return false;
}


void PCB_TUNING_PATTERN::ViewDraw( int, KIGFX::VIEW* ) const
{
}


void PCB_TUNING_PATTERN::ShowPropertiesDialog( PCB_BASE_EDIT_FRAME* )
{
}


std::vector<EDA_ITEM*> PCB_TUNING_PATTERN::GetPreviewItems( GENERATOR_TOOL*, PCB_BASE_EDIT_FRAME*, bool )
{
    return {};
}
