// Window-less KiCad program and module objects shared by the probes.
//
// This file adapts GPL-3.0-or-later KiCad code; see probe.cpp.
#pragma once

#include <kiface_base.h>
#include <kiplatform/app.h>
#include <pgm_base.h>
#include <settings/kicad_settings.h>
#include <settings/settings_manager.h>

#include <wx/app.h>
#include <wx/init.h>

#include <chrono>

#include <sys/resource.h>

// The only pure virtual in PGM_BASE; everything else is KiCad's real program object.
struct HEADLESS_PGM : public PGM_BASE
{
    void MacOpenFile( const wxString& ) override {}
};

// Model code reaches its module object through Kiface(), normally defined beside the editor
// frames (eeschema/eeschema.cpp, pcbnew/pcbnew.cpp). A headless module creates no windows.
struct HEADLESS_KIFACE : public KIFACE_BASE
{
    HEADLESS_KIFACE( const char* aName, KIWAY::FACE_T aFace ) : KIFACE_BASE( aName, aFace ) {}
    bool OnKifaceStart( PGM_BASE*, int, KIWAY* ) override { return true; }
    void OnKifaceEnd() override {}
    void Reset() override {}
    void* IfaceOrAddress( int ) override { return nullptr; }
    void GetActions( std::vector<TOOL_ACTION*>& ) const override {}
    wxWindow* CreateKiWindow( wxWindow*, int, KIWAY*, int ) override { return nullptr; }
};

// Same order as KiCad's QA entry points. Register module settings before calling Load().
inline bool StartHeadless( int argc, char** argv )
{
    SetPgm( new HEADLESS_PGM() );
    KIPLATFORM::APP::Init();
    wxApp::SetInstance( new wxAppConsole );

    if( !wxInitialize( argc, argv ) )
        return false;

    Pgm().InitPgm( true, true, true );
    Pgm().GetSettingsManager().RegisterSettings( new KICAD_SETTINGS, false );
    return true;
}

inline double MsSince( std::chrono::steady_clock::time_point aStart )
{
    return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - aStart ).count();
}

// Peak resident memory of this process; Apple platforms report ru_maxrss in bytes.
inline double PeakRssMB()
{
    rusage usage{};
    getrusage( RUSAGE_SELF, &usage );
    return usage.ru_maxrss / ( 1024.0 * 1024.0 );
}
