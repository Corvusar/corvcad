/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2021 Ola Rinta-Koski
 * Copyright (C) 2023 CERN (www.cern.ch)
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

// CorvCAD replacement for KiCad 10.0.6 common/font/fontconfig.cpp on Apple platforms: the same
// FONTCONFIG interface over Core Text instead of Fontconfig, so an iPad finds outline fonts among
// the system fonts and needs neither Fontconfig nor a fonts.conf. Default KiCad text uses the
// built-in stroke font and never reaches this file.

#include <font/fontconfig.h>
#include <reporter.h>

#include <wx/intl.h>

#include <CoreText/CoreText.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <climits>
#include <cstring>
#include <mutex>
#include <set>

using namespace fontconfig;

REPORTER*         FONTCONFIG::s_reporter = nullptr;
static std::mutex g_fontConfigMutex;

namespace
{

std::string utf8( CFStringRef aString )
{
    if( !aString )
        return {};

    const CFIndex size = CFStringGetMaximumSizeForEncoding( CFStringGetLength( aString ),
                                                            kCFStringEncodingUTF8 ) + 1;
    std::string out( size, '\0' );

    if( !CFStringGetCString( aString, out.data(), size, kCFStringEncodingUTF8 ) )
        return {};

    out.resize( std::strlen( out.c_str() ) );
    return out;
}


// Embedded font files are registered with Core Text for this process, once each.
// Callers hold g_fontConfigMutex.
void registerFiles( const std::vector<wxString>* aFiles )
{
    static std::set<wxString> registered;

    if( !aFiles )
        return;

    for( const wxString& file : *aFiles )
    {
        if( !registered.insert( file ).second )
            continue;

        const wxScopedCharBuffer path = file.utf8_str();
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
                nullptr, reinterpret_cast<const UInt8*>( path.data() ), path.length(), false );
        CTFontManagerRegisterFontsForURL( url, kCTFontManagerScopeProcess, nullptr );
        CFRelease( url );
    }
}


std::string fontFile( CTFontRef aFont )
{
    CFURLRef url = static_cast<CFURLRef>( CTFontCopyAttribute( aFont, kCTFontURLAttribute ) );

    if( !url )
        return {};

    char       path[PATH_MAX];
    const bool ok = CFURLGetFileSystemRepresentation( url, true, reinterpret_cast<UInt8*>( path ),
                                                      sizeof( path ) );
    CFRelease( url );
    return ok ? std::string( path ) : std::string();
}


// Core Text names a face by its PostScript name; FreeType wants its index in a collection (.ttc).
// Callers hold g_fontConfigMutex, which also serialises the FreeType library.
int faceIndex( const std::string& aFile, CTFontRef aFont )
{
    static FT_Library library = nullptr;
    FT_Face           face = nullptr;

    if( ( !library && FT_Init_FreeType( &library ) ) || FT_New_Face( library, aFile.c_str(), -1, &face ) )
        return 0;

    const FT_Long count = face->num_faces;
    FT_Done_Face( face );

    CFStringRef       postScriptName = CTFontCopyPostScriptName( aFont );
    const std::string wanted = utf8( postScriptName );
    CFRelease( postScriptName );

    for( FT_Long i = 0; count > 1 && i < count; ++i )
    {
        if( FT_New_Face( library, aFile.c_str(), i, &face ) )
            continue;

        const char* name = FT_Get_Postscript_Name( face );
        const bool  match = name && wanted == name;
        FT_Done_Face( face );

        if( match )
            return static_cast<int>( i );
    }

    return 0;
}


// The named family, or Core Text's fallback for an unknown one, in the closest available style.
CTFontRef createFont( const wxString& aFamily, bool aBold, bool aItalic )
{
    CFStringRef family = CFStringCreateWithCString( nullptr, aFamily.utf8_str(), kCFStringEncodingUTF8 );
    const void* keys[] = { kCTFontFamilyNameAttribute };
    const void* values[] = { family };
    CFDictionaryRef attributes = CFDictionaryCreate( nullptr, keys, values, 1,
                                                     &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks );
    CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes( attributes );
    CTFontRef           font = CTFontCreateWithFontDescriptor( descriptor, 0.0, nullptr );
    CFRelease( descriptor );
    CFRelease( attributes );
    CFRelease( family );

    const CTFontSymbolicTraits bold = aBold ? kCTFontBoldTrait : 0;
    const CTFontSymbolicTraits italic = aItalic ? kCTFontItalicTrait : 0;

    for( const CTFontSymbolicTraits traits : { bold | italic, bold, italic } )
    {
        if( !traits )
            continue;

        if( CTFontRef styled = CTFontCreateCopyWithSymbolicTraits( font, 0.0, nullptr, traits, traits ) )
        {
            CFRelease( font );
            return styled;
        }
    }

    return font;
}

} // namespace


wxString FONTCONFIG::Version()
{
    return wxS( "Core Text" );
}


FONTCONFIG::FONTCONFIG()
{
}


void FONTCONFIG::SetReporter( REPORTER* aReporter )
{
    std::lock_guard lock( g_fontConfigMutex );
    s_reporter = aReporter;
}


REPORTER* FONTCONFIG::GetReporter()
{
    std::lock_guard lock( g_fontConfigMutex );
    return s_reporter;
}


FONTCONFIG* Fontconfig()
{
    static FONTCONFIG* config = new FONTCONFIG();
    return config;
}


FONTCONFIG::FF_RESULT FONTCONFIG::FindFont( const wxString& aFontName, wxString& aFontFile,
                                            int& aFaceIndex, bool aBold, bool aItalic,
                                            const std::vector<wxString>* aEmbeddedFiles )
{
    const wxString lowerName = aFontName.Lower();

    // As upstream: a name that says it is bold is bold, whatever was asked.
    for( const wxString word : { wxS( "bold" ), wxS( "heavy" ), wxS( "black" ), wxS( "thick" ), wxS( "dark" ) } )
    {
        if( lowerName.Contains( word ) )
            aBold = true;
    }

    std::lock_guard lock( g_fontConfigMutex );
    registerFiles( aEmbeddedFiles );

    CTFontRef         font = createFont( aFontName, aBold, aItalic );
    const std::string file = fontFile( font );
    CFStringRef       familyRef = CTFontCopyFamilyName( font );
    CFStringRef       fullNameRef = CTFontCopyFullName( font );
    const wxString    family = wxString::FromUTF8( utf8( familyRef ) );
    const wxString    fullName = wxString::FromUTF8( utf8( fullNameRef ) );
    const CTFontSymbolicTraits traits = CTFontGetSymbolicTraits( font );
    CFRelease( familyRef );
    CFRelease( fullNameRef );

    FF_RESULT retval = FF_RESULT::FF_ERROR;

    if( !file.empty() )
    {
        aFontFile = wxString::FromUTF8( file );
        aFaceIndex = faceIndex( file, font );

        const bool hasBold = traits & kCTFontBoldTrait;
        const bool hasItal = traits & kCTFontItalicTrait;

        if( !family.Lower().StartsWith( lowerName ) && fullName.CmpNoCase( aFontName ) != 0 )
            retval = FF_RESULT::FF_SUBSTITUTE;
        else if( aBold && !hasBold && aItalic && !hasItal )
            retval = FF_RESULT::FF_MISSING_BOLD_ITAL;
        else if( aBold && !hasBold )
            retval = FF_RESULT::FF_MISSING_BOLD;
        else if( aItalic && !hasItal )
            retval = FF_RESULT::FF_MISSING_ITAL;
        else
            retval = FF_RESULT::FF_OK;
    }

    CFRelease( font );

    if( s_reporter && retval == FF_RESULT::FF_ERROR )
        s_reporter->Report( wxString::Format( _( "Error loading font '%s'." ), aFontName ) );
    else if( s_reporter && retval == FF_RESULT::FF_SUBSTITUTE )
        s_reporter->Report( wxString::Format( _( "Font '%s' not found; substituting '%s'." ),
                                              aFontName, fullName ) );

    return retval;
}


void FONTCONFIG::ListFonts( std::vector<std::string>& aFonts, const std::string& aDesiredLang,
                            const std::vector<wxString>* aEmbeddedFiles, bool aForce )
{
    std::lock_guard lock( g_fontConfigMutex );

    if( m_fontInfoCache.empty() || m_fontCacheLastLang != aDesiredLang || aForce )
    {
        registerFiles( aEmbeddedFiles );

        // ponytail: family names only, as Core Text localises them, not per aDesiredLang and without
        // per-face file and style; the cache only feeds the desktop font picker. Fill those in
        // when the app grows a font picker.
        CFArrayRef families = CTFontManagerCopyAvailableFontFamilyNames();

        for( CFIndex i = 0; families && i < CFArrayGetCount( families ); ++i )
        {
            std::string family = utf8( static_cast<CFStringRef>( CFArrayGetValueAtIndex( families, i ) ) );

            if( !family.empty() && family.front() != '.' )
                m_fontInfoCache.emplace( family, FONTINFO( "", "", family ) );
        }

        if( families )
            CFRelease( families );

        m_fontCacheLastLang = aDesiredLang;
    }

    for( const std::pair<const std::string, FONTINFO>& entry : m_fontInfoCache )
        aFonts.push_back( entry.second.Family() );
}
