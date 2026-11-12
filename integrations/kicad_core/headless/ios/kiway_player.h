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

// CorvCAD iOS stand-in for KiCad 10.0.6 include/kiway_player.h; see eda_base_frame.h.

#ifndef KIWAY_PLAYER_H_
#define KIWAY_PLAYER_H_

#include <eda_base_frame.h>

// Same values as upstream: model code passes them to KIWAY_PLAYER::OpenProjectFiles().
#define KICTL_NONKICAD_ONLY     (1<<0)   ///< chosen file is non-KiCad according to user
#define KICTL_KICAD_ONLY        (1<<1)   ///< chosen file is from KiCad according to user
#define KICTL_CREATE            (1<<2)   ///< caller thinks requested project files may not exist.
#define KICTL_IMPORT_LIB        (1<<3)   ///< import all footprints into a project library.
#define KICTL_REVERT            (1<<4)   ///< reverting to a previously-saved (KiCad) file.

class KIWAY_PLAYER : public EDA_BASE_FRAME
{
};

#endif  // KIWAY_PLAYER_H_