/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2015 Mark Roszko <mark.roszko@gmail.com>
 * Copyright (C) 2016 SoftPLC Corporation, Dick Hollenbeck <dick@softplc.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

// CorvCAD headless replacement for common/kicad_curl/kicad_curl.cpp and kicad_curl_easy.cpp
// (KiCad 10.0.6), without libcurl. The slice reaches curl through PGM_BASE (global init) and
// HTTP symbol libraries. Requests never run: Perform() fails and the response status is 0, so
// HTTP_LIB_CONNECTION reports a server error and an HTTP library loads no parts.

#include <kicad_curl/kicad_curl.h>
#include <kicad_curl/kicad_curl_easy.h>

// Defined in kicad_curl_easy.cpp; never allocated here.
struct CURL_PROGRESS
{
};


void KICAD_CURL::Init()
{
}


void KICAD_CURL::Cleanup()
{
}


std::string GetKicadCurlVersion()
{
    return "KiCad curl: not available in this build";
}


std::string GetCurlLibVersion()
{
    return "not available";
}


KICAD_CURL_EASY::KICAD_CURL_EASY() :
        m_CURL( nullptr ),
        m_headers( nullptr )
{
}


KICAD_CURL_EASY::~KICAD_CURL_EASY()
{
}


int KICAD_CURL_EASY::Perform()
{
    return 1;   // CURLE_UNSUPPORTED_PROTOCOL
}


void KICAD_CURL_EASY::SetHeader( const std::string&, const std::string& )
{
}


bool KICAD_CURL_EASY::SetURL( const std::string& )
{
    return false;
}


bool KICAD_CURL_EASY::SetFollowRedirects( bool )
{
    return false;
}


int KICAD_CURL_EASY::GetResponseStatusCode()
{
    return 0;
}
