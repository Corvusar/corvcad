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

// CorvCAD iOS stand-in for KiCad 10.0.6 include/eda_base_frame.h. The real frame is a wxFrame
// with AUI panes and menus, which wxWidgets' iOS port does not provide. Headless no frame is
// ever created; model code only needs the type, the members it calls, and the non-GUI
// headers the real file brings in (model files rely on those transitively).

#ifndef  EDA_BASE_FRAME_H_
#define  EDA_BASE_FRAME_H_

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json_fwd.hpp>
#include <wx/datetime.h>
#include <wx/debug.h>
#include <wx/tokenzr.h>
#include <wx/window.h>

#include <frame_type.h>
#include <i18n_utility.h>
#include <ki_exception.h>
#include <kiway_holder.h>
#include <layer_ids.h>
#include <origin_transforms.h>
#include <tool/tools_holder.h>
#include <ui_events.h>
#include <undo_redo_container.h>
#include <units_provider.h>
#include <widgets/ui_common.h>

class EDA_DRAW_PANEL_GAL;

class EDA_BASE_FRAME : public wxWindow, public TOOLS_HOLDER, public KIWAY_HOLDER,
                       public UNITS_PROVIDER
{
public:
    bool    IsType( FRAME_T aType ) const { return m_ident == aType; }
    FRAME_T GetFrameType() const { return m_ident; }

protected:
    FRAME_T m_ident;
};

#endif  // EDA_BASE_FRAME_H_