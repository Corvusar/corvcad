/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2015 Jean-Pierre Charras, jp.charras at wanadoo.fr
 * Copyright (C) 2015 CERN
 * Copyright (C) 2012 SoftPLC Corporation, Dick Hollenbeck <dick@softplc.com>
 * Copyright (C) 2011 Wayne Stambaugh <stambaughw@gmail.com>
 *
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

// CorvCAD headless replacement for pcbnew/netlist_reader/board_netlist_updater_frame.cpp
// (KiCad 10.0.6 with patch 0009). The headless slice has no board editor: BOARD_NETLIST_UPDATER
// runs with a null frame and loads footprints through SetFootprintLoader(). It reaches
// PositionText(), which formats millimetres as upstream does without a frame, and
// ExchangeFootprint(), which runs PCB_EDIT_FRAME::ExchangeFootprint's headless copy
// (exchange_footprint.cpp). The others are never called without a frame and do nothing.

#include <netlist_reader/board_netlist_updater.h>

#include <board.h>
#include <eda_units.h>
#include <footprint.h>
#include <base_units.h>


// exchange_footprint.cpp.
void CorvCadExchangeFootprint( BOARD* aBoard, FOOTPRINT* aExisting, FOOTPRINT* aNew, BOARD_COMMIT& aCommit );

namespace BOARD_NETLIST_UPDATER_FRAME
{

FOOTPRINT* LoadFootprint( PCB_EDIT_FRAME*, const LIB_ID& )
{
    return nullptr;
}


void ExchangeFootprint( PCB_EDIT_FRAME*, FOOTPRINT* aExisting, FOOTPRINT* aNew, BOARD_COMMIT& aCommit )
{
    CorvCadExchangeFootprint( aExisting->GetBoard(), aExisting, aNew, aCommit );
}


bool ShowGlobalRatsnest( PCB_EDIT_FRAME* )
{
    return true;
}


void StyleField( PCB_EDIT_FRAME*, PCB_FIELD* )
{
}


void RemoveFromView( PCB_EDIT_FRAME*, EDA_ITEM* )
{
}


void OnModify( PCB_EDIT_FRAME* )
{
}


std::pair<wxString, wxString> PositionText( PCB_EDIT_FRAME*, VECTOR2I aPosition )
{
    return { EDA_UNIT_UTILS::UI::MessageTextFromValue( pcbIUScale, EDA_UNITS::MM, aPosition.x ),
             EDA_UNIT_UTILS::UI::MessageTextFromValue( pcbIUScale, EDA_UNITS::MM, aPosition.y ) };
}

} // namespace BOARD_NETLIST_UPDATER_FRAME
