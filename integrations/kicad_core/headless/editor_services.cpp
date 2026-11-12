/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Mark Roszko <mark.roszko@gmail.com>
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

// CorvCAD headless replacements for the editor services PGM_BASE owns (KiCad 10.0.6):
// common/background_jobs_monitor.cpp, common/notifications_manager.cpp and the two
// KISTATUSBAR calls in pgm_base.cpp. Headless there are no status bars, job lists or
// notification popups, so these keep only bookkeeping.

#include <wx/filename.h>

#include <background_jobs_monitor.h>
#include <notifications_manager.h>
#include <widgets/kistatusbar.h>

#include <algorithm>
#include <mutex>


BACKGROUND_JOBS_MONITOR::BACKGROUND_JOBS_MONITOR()
{
}


// ponytail: jobs carry no reporter, because BACKGROUND_JOB_REPORTER is a GUI progress reporter;
// only the editor's library preload reports through one.
std::shared_ptr<BACKGROUND_JOB> BACKGROUND_JOBS_MONITOR::Create( const wxString& aName )
{
    std::shared_ptr<BACKGROUND_JOB> job = std::make_shared<BACKGROUND_JOB>();
    job->m_name = aName;

    std::unique_lock<std::shared_mutex> lock( m_mutex );
    m_jobs.push_back( job );
    return job;
}


void BACKGROUND_JOBS_MONITOR::Remove( std::shared_ptr<BACKGROUND_JOB> aJob )
{
    std::unique_lock<std::shared_mutex> lock( m_mutex );
    m_jobs.erase( std::remove( m_jobs.begin(), m_jobs.end(), aJob ), m_jobs.end() );
}


NOTIFICATIONS_MANAGER::NOTIFICATIONS_MANAGER()
{
}


void NOTIFICATIONS_MANAGER::Load()
{
}


// PGM_BASE forwards library load messages to registered status bars; headless none register.
void KISTATUSBAR::AddWarningMessages( const wxString&, const std::vector<LOAD_MESSAGE>& )
{
}


void KISTATUSBAR::ClearWarningMessages( const wxString& )
{
}
