/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.TXT for contributors.
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
 * http://www.gnu.org/licenses/gpl-3.0.html
 * or you may search the http://www.gnu.org website for the version 3 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */
// CorvCAD iOS stand-in for KiCad 10.0.6 common/git/project_git_utils.h, without git2.h. PROJECT
// only asks for the current hash (headless/text_eval_vcs.cpp).

#ifndef PROJECT_GIT_UTILS_H
#define PROJECT_GIT_UTILS_H

#include <wx/string.h>
#include <import_export.h>

namespace KIGIT
{

class APIEXPORT PROJECT_GIT_UTILS
{
public:
    static wxString GetCurrentHash( const wxString& aProjectFile, bool aShort );
};

} // namespace KIGIT

#endif // PROJECT_GIT_UTILS_H
