/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2016 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * https://www.gnu.org/licenses/gpl-3.0.html
 * or you may search the http://www.gnu.org website for the version 3 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

// CorvCAD headless replacement for eeschema/sim/spice_simulator.cpp (KiCad 10.0.6): no ngspice
// is linked. Only KIBIS_PIN::getKuKdFromFile reaches this, and it reports a missing simulator
// as an error. The probes drop that path with -dead_strip; an app's debug dylib keeps it, so
// the app package (probe/ios_package.sh) links this.

#include <string>

class wxString; // simulator.h uses it by reference only; this file needs no wx headers.

#include <simulator.h>

std::shared_ptr<SPICE_SIMULATOR> SIMULATOR::CreateInstance( const std::string& )
{
    return nullptr;
}
