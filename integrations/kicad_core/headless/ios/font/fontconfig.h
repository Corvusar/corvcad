/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2021 Ola Rinta-Koski
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
// CorvCAD iOS stand-in for KiCad 10.0.6 include/font/fontconfig.h, without
// <fontconfig/fontconfig.h>. The slice's implementation is headless/fontconfig.cpp (Core Text);
// the class layout matches upstream.

#ifndef KICAD_FONTCONFIG_H
#define KICAD_FONTCONFIG_H

#include <kicommon.h>
#include <wx/string.h>
#include <vector>
#include <map>
#include <unordered_map>
#include <font/fontinfo.h>

class REPORTER;
namespace fontconfig
{

struct FONTCONFIG_PAT;

class KICOMMON_API FONTCONFIG
{
public:
    FONTCONFIG();

    static wxString Version();

    enum class FF_RESULT
    {
        FF_OK,
        FF_ERROR,
        FF_SUBSTITUTE,
        FF_MISSING_BOLD,
        FF_MISSING_ITAL,
        FF_MISSING_BOLD_ITAL
    };

    FF_RESULT FindFont( const wxString& aFontName, wxString& aFontFile, int& aFaceIndex, bool aBold,
                        bool aItalic, const std::vector<wxString>* aEmbeddedFiles = nullptr );

    void ListFonts( std::vector<std::string>& aFonts, const std::string& aDesiredLang,
                    const std::vector<wxString>* aEmbeddedFiles = nullptr, bool aForce = false );

    static void SetReporter( REPORTER* aReporter );

    static REPORTER* GetReporter();

private:
    std::map<std::string, FONTINFO> m_fontInfoCache;
    wxString                        m_fontCacheLastLang;
    static REPORTER*                s_reporter;

    bool isLanguageMatch( const wxString& aSearchLang, const wxString& aSupportedLang );

    void getAllFamilyStrings( FONTCONFIG_PAT&                               aPat,
                              std::unordered_map<std::string, std::string>& aFamStringMap );

    std::string getFamilyStringByLang( FONTCONFIG_PAT& APat, const wxString& aDesiredLang );

    std::string getFcString( FONTCONFIG_PAT& aPat, const char* aObj, int aIdx );
};

} // namespace fontconfig


KICOMMON_API fontconfig::FONTCONFIG* Fontconfig();


#endif //KICAD_FONTCONFIG_H
