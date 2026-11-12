/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2022 Jon Evans <jon@craftyjon.com>
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

// CorvCAD headless replacement for common/database/database_connection.cpp (KiCad 10.0.6),
// without nanodbc and ODBC. Database symbol libraries never connect, so SCH_IO_DATABASE
// reports the error below and the library loads no parts.

#include <database/database_connection.h>

// Defined by nanodbc; never allocated here.
namespace nanodbc
{
class connection
{
};
}


DATABASE_CONNECTION::DATABASE_CONNECTION( const std::string& aDataSourceName,
                                          const std::string& aUsername,
                                          const std::string& aPassword, int aTimeoutSeconds,
                                          bool aConnectNow ) :
        m_dsn( aDataSourceName ),
        m_user( aUsername ),
        m_pass( aPassword ),
        m_timeout( aTimeoutSeconds ),
        m_quoteChar( '"' )
{
    m_lastError = "Database libraries are not available in this build";
}


DATABASE_CONNECTION::DATABASE_CONNECTION( const std::string& aConnectionString,
                                          int aTimeoutSeconds, bool aConnectNow ) :
        m_connectionString( aConnectionString ),
        m_timeout( aTimeoutSeconds ),
        m_quoteChar( '"' )
{
    m_lastError = "Database libraries are not available in this build";
}


DATABASE_CONNECTION::~DATABASE_CONNECTION()
{
}


void DATABASE_CONNECTION::SetCacheParams( int, int )
{
}


bool DATABASE_CONNECTION::IsConnected() const
{
    return false;
}


bool DATABASE_CONNECTION::CacheTableInfo( const std::string&, const std::set<std::string>&,
                                          const std::set<std::string>& )
{
    return false;
}


bool DATABASE_CONNECTION::SelectOne( const std::string&,
                                     const std::pair<std::string, std::string>&, ROW& )
{
    return false;
}


bool DATABASE_CONNECTION::SelectAll( const std::string&, const std::string&,
                                     std::vector<ROW>& )
{
    return false;
}
