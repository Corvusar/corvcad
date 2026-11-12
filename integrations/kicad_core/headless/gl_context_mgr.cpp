/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2016 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Maciej Suminski <maciej.suminski@cern.ch>
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

// CorvCAD headless replacement for common/kicad_gl/gl_context_mgr.cpp (KiCad 10.0.6).
// KICAD_SINGLETON owns a GL_CONTEXT_MANAGER, but headless there are no canvases, so no
// context is ever created and nothing needs locking.

#include <kicad_gl/gl_context_mgr.h>


wxGLContext* GL_CONTEXT_MANAGER::CreateCtx( wxGLCanvas*, const wxGLContext* )
{
    return nullptr;
}


void GL_CONTEXT_MANAGER::DestroyCtx( wxGLContext* )
{
}


void GL_CONTEXT_MANAGER::DeleteAll()
{
    m_glContexts.clear();
    m_glCtx = nullptr;
}


void GL_CONTEXT_MANAGER::LockCtx( wxGLContext*, wxGLCanvas* )
{
}


void GL_CONTEXT_MANAGER::UnlockCtx( wxGLContext* )
{
}