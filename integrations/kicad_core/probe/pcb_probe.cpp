// SPDX-License-Identifier: GPL-3.0-or-later
// CorvCAD KiCad 10.0.6 board probe (port gate 2).
//
// Loads a board through kicad-cli's own loader (pcbnew_scripting_helpers.cpp LoadBoard),
// writes the same JSON report as integrations/kicad_native/bridge.py `inspect`, saves the
// board through KiCad's writer (SaveBoard), then runs DRC as kicad-cli's JobExportDrc does
// and writes its JSON report.
//
// This file adapts GPL-3.0-or-later KiCad code; see probe.cpp.
//
//   corvcad_kicad_pcb_probe <in.kicad_pcb> <out-dir>

#include "headless.hpp"

#include <base_units.h>
#include <board.h>
#include <board_design_settings.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <drc/drc_report.h>
#include <footprint.h>
#include <footprint_editor_settings.h>
#include <footprint_library_adapter.h>
#include <pad.h>
#include <pcb_marker.h>
#include <project_pcb.h>
#include <pcb_track.h>
#include <pcbnew_scripting_helpers.h>
#include <pcbnew_settings.h>
#include <zone.h>

#include <nlohmann/json.hpp>
#include <wx/filename.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>

KIFACE_BASE& Kiface()
{
    static HEADLESS_KIFACE kiface( "pcbnew", KIWAY::FACE_PCB );
    return kiface;
}

namespace {

using nlohmann::json;

// bridge.py: round( pcbnew.ToMM( value ), 6 )
double mm( int aValue )
{
    return std::round( pcbIUScale.IUTomm( aValue ) * 1e6 ) / 1e6;
}

json point( const VECTOR2I& aValue )
{
    return json::array( { mm( aValue.x ), mm( aValue.y ) } );
}

std::string utf8( const wxString& aText )
{
    return std::string( aText.ToUTF8() );
}

json inspect( BOARD& aBoard )
{
    json footprints = json::array();

    for( FOOTPRINT* footprint : aBoard.Footprints() )
    {
        json pads = json::array();

        for( PAD* pad : footprint->Pads() )
        {
            pads.push_back( { { "number", utf8( pad->GetNumber() ) },
                              { "position_mm", point( pad->GetPosition() ) },
                              { "net", utf8( pad->GetNetname() ) } } );
        }

        std::stable_sort( pads.begin(), pads.end(), []( const json& a, const json& b )
                          { return a["number"] < b["number"]; } );
        footprints.push_back( { { "reference", utf8( footprint->GetReference() ) },
                                { "value", utf8( footprint->GetValue() ) },
                                { "position_mm", point( footprint->GetPosition() ) },
                                { "rotation_deg", footprint->GetOrientationDegrees() },
                                { "pads", pads } } );
    }

    std::stable_sort( footprints.begin(), footprints.end(), []( const json& a, const json& b )
                      { return a["reference"] < b["reference"]; } );

    json tracks = json::array();
    json vias = json::array();

    for( PCB_TRACK* item : aBoard.Tracks() )
    {
        if( item->Type() == PCB_VIA_T )
        {
            vias.push_back( { { "position_mm", point( item->GetPosition() ) },
                              { "net", utf8( item->GetNetname() ) } } );
        }
        else
        {
            tracks.push_back( { { "start_mm", point( item->GetStart() ) },
                                { "end_mm", point( item->GetEnd() ) },
                                { "net", utf8( item->GetNetname() ) },
                                { "layer", utf8( item->GetLayerName() ) } } );
        }
    }

    return { { "footprints", footprints },
             { "tracks", tracks },
             { "vias", vias },
             { "zones", aBoard.Zones().size() } };
}

} // namespace

int main( int argc, char** argv )
{
    if( argc != 3 )
    {
        std::fprintf( stderr, "usage: %s <in.kicad_pcb> <out-dir>\n", argv[0] );
        return 2;
    }

    if( !StartHeadless( argc, argv ) )
        return 3;

    SETTINGS_MANAGER& manager = Pgm().GetSettingsManager();
    manager.RegisterSettings( new PCBNEW_SETTINGS, false );
    manager.RegisterSettings( new FOOTPRINT_EDITOR_SETTINGS, false );
    manager.Load();

    const auto start = std::chrono::steady_clock::now();
    const wxString input = wxString::FromUTF8( argv[1] );
    const wxString outDir = wxString::FromUTF8( argv[2] );

    wxString reason;
    std::unique_ptr<BOARD> board( LoadBoard( input, true, &reason ) );

    if( !board )
    {
        std::fprintf( stderr, "load failed: %s\n", reason.ToUTF8().data() );
        return 4;
    }

    const double loadMs = MsSince( start );
    std::ofstream( ( outDir + wxS( "/board.json" ) ).ToStdString() ) << inspect( *board ).dump( 2 ) << "\n";

    // Board file only; the project file beside it stays as copied.
    wxString saved = wxFileName( outDir, wxFileName( input ).GetFullName() ).GetFullPath();

    if( !SaveBoard( saved, board.get(), true ) )
    {
        std::fprintf( stderr, "save failed: %s\n", saved.ToUTF8().data() );
        return 5;
    }

    // kicad-cli's JobExportDrc with its defaults: no schematic parity, no zone refill, errors
    // and warnings, millimetres. LoadBoard already created the engine with the project rules.
    // kicad-cli collects markers through BOARD_COMMIT, which needs a TOOL_MANAGER; the probe
    // adds them to the board after the run instead.
    const auto drcStart = std::chrono::steady_clock::now();
    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( board->GetProject() );
    adapter->AsyncLoad();
    adapter->BlockUntilLoaded();

    DS_PROXY_VIEW_ITEM drawingSheet( pcbIUScale, &board->GetPageSettings(), board->GetProject(),
                                     &board->GetTitleBlock(), &board->GetProperties() );
    drawingSheet.SetSheetName( std::string() );
    drawingSheet.SetSheetPath( std::string() );
    drawingSheet.SetIsFirstPage( true );
    drawingSheet.SetFileName( utf8( board->GetFileName() ) );
    drawingSheet.SetVariantName( utf8( board->GetCurrentVariant() ) );
    drawingSheet.SetVariantDesc( utf8( board->GetVariantDescription( board->GetCurrentVariant() ) ) );

    std::shared_ptr<DRC_ENGINE> drc = board->GetDesignSettings().m_DRCEngine;
    std::vector<PCB_MARKER*>    markers;
    drc->SetDrawingSheet( &drawingSheet );
    drc->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                 const std::function<void( PCB_MARKER* )>& aPathGenerator )
            {
                markers.push_back( new PCB_MARKER( aItem, aPos, aLayer ) );
                aPathGenerator( markers.back() );
            } );
    board->RecordDRCExclusions();
    board->DeleteMARKERs( true, true );
    drc->RunTests( EDA_UNITS::MM, false, false );
    drc->ClearViolationHandler();
    drc->SetDrawingSheet( nullptr );

    for( PCB_MARKER* marker : markers )
        board->Add( marker );

    board->ResolveDRCExclusions( false );

    auto violations = std::make_shared<DRC_ITEMS_PROVIDER>( board.get(), MARKER_BASE::MARKER_DRC,
                                                            MARKER_BASE::MARKER_DRAWING_SHEET );
    auto unconnected = std::make_shared<DRC_ITEMS_PROVIDER>( board.get(), MARKER_BASE::MARKER_RATSNEST );
    auto parity = std::make_shared<DRC_ITEMS_PROVIDER>( board.get(), MARKER_BASE::MARKER_PARITY );

    for( const auto& provider : { violations, unconnected, parity } )
        provider->SetSeverities( RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING );

    DRC_REPORT( board.get(), EDA_UNITS::MM, violations, unconnected, parity )
            .WriteJsonReport( outDir + wxS( "/probe-drc.json" ) );
    const double drcMs = MsSince( drcStart );

    std::printf( "{\"footprints\":%zu,\"tracks\":%zu,\"nets\":%u,\"drcViolations\":%d,"
                 "\"unconnected\":%d,\"loadMs\":%.1f,\"drcMs\":%.1f,\"totalMs\":%.1f,\"peakMB\":%.1f}\n",
                 board->Footprints().size(), board->Tracks().size(), board->GetNetCount(),
                 violations->GetCount(), unconnected->GetCount(), loadMs, drcMs, MsSince( start ),
                 PeakRssMB() );

    board.reset();
    Pgm().Destroy();
    wxUninitialize();
    return 0;
}
