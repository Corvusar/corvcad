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

// CorvCAD headless replacement for common/text_eval/text_eval_vcs.cpp and the one
// KIGIT::PROJECT_GIT_UTILS function the slice uses (KiCad 10.0.6), without libgit2. Every
// query answers as upstream does outside a git repository, so text variables such as
// ${VCSHASH} and @{vcs...} expressions evaluate as they would for an unversioned project.

#include <text_eval/text_eval_vcs.h>
#include <git/project_git_utils.h>


namespace TEXT_EVAL_VCS
{

void SetContextPath( const wxString& )
{
}


std::string GetCommitHash( const std::string&, int )
{
    return std::string();
}


std::string GetNearestTag( const std::string&, bool )
{
    return std::string();
}


int GetDistanceFromTag( const std::string&, bool )
{
    return 0;
}


bool IsDirty( bool )
{
    return false;
}


std::string GetAuthor( const std::string& )
{
    return std::string();
}


std::string GetAuthorEmail( const std::string& )
{
    return std::string();
}


std::string GetCommitter( const std::string& )
{
    return std::string();
}


std::string GetCommitterEmail( const std::string& )
{
    return std::string();
}


std::string GetBranch()
{
    return std::string();
}


int64_t GetCommitTimestamp( const std::string& )
{
    return 0;
}

} // namespace TEXT_EVAL_VCS


wxString KIGIT::PROJECT_GIT_UTILS::GetCurrentHash( const wxString&, bool )
{
    return wxT( "no hash" );
}
