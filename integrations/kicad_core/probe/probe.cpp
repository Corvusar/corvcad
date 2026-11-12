// SPDX-License-Identifier: GPL-3.0-or-later
// CorvCAD KiCad 10.0.6 schematic probe (port gate 2).
//
// Loads a schematic and rebuilds hierarchy and connectivity through kicad-cli's own loader
// (eeschema/eeschema_helpers.cpp LoadSchematic), exports a KiCad netlist, runs ERC as
// kicad-cli's JobSchErc does and writes its JSON report, and saves the top-level sheet
// through KiCad's SCH_IO writer.
//
// This file adapts GPL-3.0-or-later KiCad code; a binary linking KiCad sources is a
// GPL combined work. It is a build proof only and is not linked into the iPad app.
//
//   corvcad_kicad_probe <in.kicad_sch> <out-dir>
//
// With CORVCAD_PROBE_FONTS="Helvetica,Times Bold,..." it also resolves each name through KiCad's
// font loader and prints the outline face it loaded, or that it fell back to the stroke font.
// In the simulator, pass it as SIMCTL_CHILD_CORVCAD_PROBE_FONTS.

#include "headless.hpp"

#include <connection_graph.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <eeschema_helpers.h>
#include <erc/erc.h>
#include <erc/erc_report.h>
#include <erc/erc_settings.h>
#include <libraries/symbol_library_adapter.h>
#include <project_sch.h>
#include <eeschema_settings.h>
#include <font/outline_font.h>
#include <io/io_mgr.h>
#include <netlist_exporters/netlist_exporter_kicad.h>
#include <richio.h>
#include <sch_io/sch_io.h>
#include <sch_io/sch_io_mgr.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <schematic.h>
#include <symbol_editor/symbol_editor_settings.h>

#include <wx/arrstr.h>
#include <wx/filename.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>

KIFACE_BASE& Kiface()
{
    static HEADLESS_KIFACE kiface( "eeschema", KIWAY::FACE_SCH );
    return kiface;
}

int main( int argc, char** argv )
{
    if( argc != 3 )
    {
        std::fprintf( stderr, "usage: %s <in.kicad_sch> <out-dir>\n", argv[0] );
        return 2;
    }

    if( !StartHeadless( argc, argv ) )
        return 3;

    SETTINGS_MANAGER& manager = Pgm().GetSettingsManager();
    manager.RegisterSettings( new EESCHEMA_SETTINGS, false );
    manager.RegisterSettings( new SYMBOL_EDITOR_SETTINGS, false );
    manager.Load();

    const auto start = std::chrono::steady_clock::now();
    const wxString input = wxString::FromUTF8( argv[1] );
    const wxString outDir = wxString::FromUTF8( argv[2] );

    // kicad-cli's loader: hierarchy, instance repair, variants, page numbers, connectivity.
    std::unique_ptr<SCHEMATIC> schematic( EESCHEMA_HELPERS::LoadSchematic(
            input, SCH_IO_MGR::SCH_KICAD, true, false, nullptr, true ) );

    if( !schematic )
    {
        std::fprintf( stderr, "load failed: %s\n", argv[1] );
        return 4;
    }

    SCH_SHEET_LIST sheets = schematic->Hierarchy();
    const double loadMs = MsSince( start );

    NETLIST_EXPORTER_KICAD exporter( schematic.get() );
    STRING_FORMATTER formatter;
    exporter.Format( &formatter, GNL_ALL | GNL_OPT_KICAD );
    std::ofstream( ( outDir + wxS( "/probe.net" ) ).ToStdString() ) << formatter.GetString();

    wxFileName saved( outDir, wxFileName( input ).GetFullName() );
    IO_RELEASER<SCH_IO> io( SCH_IO_MGR::FindPlugin( SCH_IO_MGR::SCH_KICAD ) );
    io->SaveSchematicFile( saved.GetFullPath(), schematic->GetTopLevelSheet(), schematic.get() );

    // kicad-cli's JobSchErc with its defaults: errors and warnings, millimetres, no footprint
    // link check (that needs the CvPcb module).
    const auto ercStart = std::chrono::steady_clock::now();
    SCH_SCREEN* root = schematic->RootScreen();
    auto markers = std::make_shared<SHEETLIST_ERC_ITEMS_PROVIDER>( schematic.get() );
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &schematic->Project() );
    adapter->AsyncLoad();
    adapter->BlockUntilLoaded();

    DS_PROXY_VIEW_ITEM drawingSheet( schIUScale, &root->GetPageSettings(), &schematic->Project(),
                                     &root->GetTitleBlock(), schematic->GetProperties() );
    drawingSheet.SetPageNumber( TO_UTF8( root->GetPageNumber() ) );
    drawingSheet.SetSheetCount( root->GetPageCount() );
    drawingSheet.SetFileName( TO_UTF8( root->GetFileName() ) );
    drawingSheet.SetIsFirstPage( root->GetVirtualPageNumber() == 1 );
    drawingSheet.SetVariantName( TO_UTF8( schematic->GetCurrentVariant() ) );
    drawingSheet.SetVariantDesc( TO_UTF8( schematic->GetVariantDescription( schematic->GetCurrentVariant() ) ) );
    drawingSheet.SetSheetName( "" );
    drawingSheet.SetSheetPath( "" );

    ERC_TESTER ercTester( schematic.get() );
    ercTester.RunTests( &drawingSheet, nullptr, nullptr, &schematic->Project(), nullptr );
    markers->SetSeverities( RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING );
    ERC_REPORT( schematic.get(), EDA_UNITS::MM, markers ).WriteJsonReport( outDir + wxS( "/probe-erc.json" ) );
    const double ercMs = MsSince( ercStart );

    std::printf( "{\"sheets\":%zu,\"nets\":%zu,\"ercViolations\":%d,\"loadMs\":%.1f,\"ercMs\":%.1f,"
                 "\"totalMs\":%.1f,\"peakMB\":%.1f}\n",
                 sheets.size(), schematic->ConnectionGraph()->GetNetMap().size(), markers->GetCount(),
                 loadMs, ercMs, MsSince( start ), PeakRssMB() );

    if( const char* fonts = std::getenv( "CORVCAD_PROBE_FONTS" ) )
    {
        for( const wxString& name : wxSplit( wxString::FromUTF8( fonts ), ',' ) )
        {
            auto* outline = dynamic_cast<KIFONT::OUTLINE_FONT*>( KIFONT::FONT::GetFont( name ) );

            if( outline )
                std::printf( "font: %s -> %s %s, face %ld of %s\n", name.utf8_str().data(),
                             outline->GetFace()->family_name, outline->GetFace()->style_name,
                             outline->GetFace()->face_index, outline->GetFileName().utf8_str().data() );
            else
                std::printf( "font: %s -> stroke font fallback\n", name.utf8_str().data() );
        }
    }

    schematic.reset();
    Pgm().Destroy();
    wxUninitialize();
    return 0;
}
