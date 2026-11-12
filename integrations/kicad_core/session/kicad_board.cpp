// SPDX-License-Identifier: GPL-3.0-or-later
// CorvCAD native board checks over KiCad 10.0.6 (port gate 5).
//
// Implements engine/include/corvcad/board_native_c_api.h on KiCad's own BOARD: loading through
// kicad-cli's loader (LoadBoard), zone fill through ZONE_FILLER and DRC as kicad-cli's
// JobExportDrc runs it (see probe/pcb_probe.cpp). The last board loaded stays open between
// calls until its file changes (openBoard). It shares the program object, settings and Kiface() with the schematic session
// (kicad_session.cpp), which every NATIVE_KICAD build links.
//
// Teardrops are regenerated before a refill, as kicad-cli's does, through the headless
// BOARD_COMMIT (headless/pcbnew/board_commit.cpp).
//
// This file adapts GPL-3.0-or-later KiCad code; a binary linking KiCad sources is a GPL
// combined work.

#include "../probe/headless.hpp"

#include <corvcad/board_native_c_api.h>

#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <teardrop/teardrop.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <drc/drc_report.h>
#include <footprint_editor_settings.h>
#include <footprint_library_adapter.h>
#include <libraries/library_manager.h>
#include <pgm_base.h>
#include <connectivity/connectivity_algo.h>
#include <connectivity/connectivity_data.h>
#include <footprint.h>
#include <footprint_import_reconciler.h>
#include <import_proj_properties.h>
#include <io/common/plugin_common_choose_project.h>
#include <pcb_dimension.h>
#include <pcb_io/pcb_io.h>
#include <pcb_io/pcb_io_mgr.h>
#include <kiface_ids.h>
#include <netclass.h>
#include <netinfo.h>
#include <project/net_settings.h>
#include <ratsnest/ratsnest_data.h>
#include <lib_id.h>
#include <netlist_reader/board_netlist_updater.h>
#include <netlist_reader/netlist_reader.h>
#include <netlist_reader/pcb_netlist.h>
#include <reporter.h>
#include <richio.h>
#include <pcb_marker.h>
#include <pcbnew_scripting_helpers.h>
#include <pcbnew_settings.h>
#include <project_pcb.h>
#include <zone.h>
#include <zone_filler.h>
#include <pcbplot.h>
#include <plotters/plotter.h>
#include <wildcards_and_files_ext.h>
#include <exporters/gendrill_excellon_writer.h>
#include <exporters/gerber_jobfile_writer.h>
#include <exporters/place_file_exporter.h>
#include <wx/dir.h>
#include <pcb_plotter.h>
#include <board_stackup_manager/board_stackup.h>
#include <eda_pattern_match.h>
#include <project/project_file.h>
#include <pad.h>
#include <pcb_track.h>
#include <lset.h>
#include <geometry/shape_arc.h>
#include <router/pns_arc.h>
#include <router/pns_diff_pair.h>
#include <router/pns_topology.h>
#include <router/pns_itemset.h>
#include <router/pns_kicad_iface.h>
#include <router/pns_line.h>
#include <router/pns_meander.h>
#include <router/pns_meander_placer_base.h>
#include <router/pns_placement_algo.h>
#include <router/pns_router.h>
#include <router/pns_routing_settings.h>
#include <router/pns_segment.h>
#include <router/pns_sizes_settings.h>
#include <router/pns_solid.h>
#include <router/pns_via.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <locale>
#include <cmath>
#include <map>
#include <tuple>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace fs = std::filesystem;

bool CorvCadStartKiCad(); // kicad_session.cpp
bool CorvCadActivateProject( const wxString& aModelFile );

namespace
{
std::string utf8( const wxString& aText ) { return aText.utf8_string(); }

void start()
{
    static const bool started = []
    {
        if( !CorvCadStartKiCad() )
            return false;

        SETTINGS_MANAGER& manager = Pgm().GetSettingsManager();
        manager.RegisterSettings( new PCBNEW_SETTINGS );
        manager.RegisterSettings( new FOOTPRINT_EDITOR_SETTINGS );
        return true;
    }();

    if( !started )
        throw std::runtime_error( "KiCad core did not start" );
}

std::unique_ptr<BOARD> load( const char* aPath )
{
    if( !aPath )
        throw std::runtime_error( "Missing board path" );

    start();

    // SETTINGS_MANAGER keeps a project it loaded once, settings included, and LoadProject
    // returns that copy. Its file may have changed since (Board Setup or its undo writes the
    // .kicad_pro), so a project already open is read again from the file first.
    wxFileName projectFile( wxString::FromUTF8( aPath ) );
    projectFile.SetExt( FILEEXT::ProjectFileExtension );

    if( PROJECT* project = Pgm().GetSettingsManager().GetProject( projectFile.GetFullPath() ) )
        project->GetProjectFile().LoadFromFile( projectFile.GetPath() );

    wxString               reason;
    const wxString         file = wxString::FromUTF8( aPath );
    std::unique_ptr<BOARD> board( LoadBoard( file, CorvCadActivateProject( file ), &reason ) );

    if( !board )
        throw std::runtime_error( "Cannot load board: " + utf8( reason ) );

    return board;
}

std::string readFile( const fs::path& aPath )
{
    std::ifstream      in( aPath, std::ios::binary );
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The board session: the last board a call loaded stays open, so the ratsnest, net rules and DRC
// after an edit share one load. It is reloaded when the file changes on disk; after an edit
// (saveText), it is kept only once the file holds exactly the returned text (the app writes it
// back).
// ponytail: one board at a time, freed only when another opens; add a close call if a closed
// project's board memory matters.
struct OPEN_BOARD
{
    std::string            path;
    fs::file_time_type     stamp;
    std::uintmax_t         size = 0;
    std::string            returned; // the text the last edit returned, until the file holds it
    std::unique_ptr<BOARD> board;
};

// Never destroyed: a BOARD freed during static destruction can outlive KiCad's settings and
// fonts (native-check trapped at exit on the video demo); the OS reclaims it.
OPEN_BOARD& g_open = *new OPEN_BOARD;

int g_loads = 0; // for board_test's session check

void endRoute(); // the router works on the open board

void closeBoard()
{
    endRoute();
    g_open = OPEN_BOARD();
}
} // namespace

// Before another project becomes active (kicad_session.cpp): the open board uses its PROJECT.
void CorvCadCloseBoard() { closeBoard(); }

// headless/pcbnew/exchange_footprint.cpp: PCB_EDIT_FRAME::ExchangeFootprint without the frame.
void CorvCadExchangeFootprint( BOARD* aBoard, FOOTPRINT* aExisting, FOOTPRINT* aNew, BOARD_COMMIT& aCommit );

namespace
{

BOARD& openBoard( const char* aPath )
{
    if( !aPath )
        throw std::runtime_error( "Missing board path" );

    std::error_code    error;
    fs::file_time_type stamp = fs::last_write_time( aPath, error );
    std::uintmax_t     size = error ? 0 : fs::file_size( aPath, error );

    if( g_open.board && g_open.path == aPath && !error )
    {
        // An edit's text the file does not hold (not written back) means a reload.
        if( g_open.returned.empty() && stamp == g_open.stamp && size == g_open.size )
            return *g_open.board;

        if( !g_open.returned.empty() && size == g_open.returned.size() && readFile( aPath ) == g_open.returned )
        {
            g_open.stamp = stamp;
            g_open.size = size;
            g_open.returned.clear();
            return *g_open.board;
        }
    }

    closeBoard(); // free the previous board before loading
    std::unique_ptr<BOARD> board = load( aPath );
    ++g_loads;
    g_open.path = aPath;
    g_open.stamp = stamp;
    g_open.size = size;
    g_open.board = std::move( board );
    return *g_open.board;
}

// ZONE_FILLER_TOOL::FillAllZones headless: teardrops are regenerated first, then every zone
// (the new teardrop areas included) is filled. The commit is headless/pcbnew/board_commit.cpp.
void fillZones( BOARD& aBoard )
{
    BOARD_COMMIT commit( static_cast<TOOL_MANAGER*>( nullptr ) );
    TEARDROP_MANAGER( &aBoard, nullptr ).UpdateTeardrops( commit, nullptr, nullptr, true /* forceFullUpdate */ );
    aBoard.IncrementTimeStamp(); // clear caches

    std::vector<ZONE*> zones( aBoard.Zones().begin(), aBoard.Zones().end() );
    ZONE_FILLER        filler( &aBoard, &commit );

    if( filler.Fill( zones ) )
        commit.Push( wxEmptyString, SKIP_CONNECTIVITY | ZONE_FILL_OP );
    else
        commit.Revert();

    aBoard.BuildConnectivity();
}

// A path in the temporary directory for KiCad's file writers, removed on scope exit.
struct TEMP_FILE
{
    explicit TEMP_FILE( const char* aExtension )
    {
        static std::atomic<int> counter{ 0 };
        path = fs::temp_directory_path()
               / ( "corvcad-board-" + std::to_string( getpid() ) + "-" + std::to_string( counter++ ) + aExtension );
    }
    ~TEMP_FILE() { std::error_code ignored; fs::remove( path, ignored ); }
    std::string read() const
    {
        std::ifstream in( path, std::ios::binary );
        std::ostringstream text;
        text << in.rdbuf();
        if( !in )
            throw std::runtime_error( "Cannot read " + path.string() );
        return text.str();
    }
    fs::path path;
};

// pcb_probe.cpp's DRC run: kicad-cli's JobExportDrc with no schematic parity, errors and
// warnings, millimetres. Markers go on the board after the run instead of through BOARD_COMMIT.
std::string drc( BOARD& aBoard )
{
    // Only the libraries the board's footprints refer to. kicad-cli loads every library of the
    // tables (AsyncLoad), over 1 GB on an iPad for the 155 official ones; the library parity
    // test only looks up the referenced libraries (IsLibraryLoaded, LoadFootprint).
    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( aBoard.GetProject() );
    std::set<wxString>         nicknames;

    for( FOOTPRINT* footprint : aBoard.Footprints() )
        nicknames.insert( wxString( footprint->GetFPID().GetLibNickname() ) );

    for( const wxString& nickname : nicknames )
    {
        if( !nickname.IsEmpty() )
            adapter->LoadOne( nickname );
    }

    DS_PROXY_VIEW_ITEM drawingSheet( pcbIUScale, &aBoard.GetPageSettings(), aBoard.GetProject(),
                                     &aBoard.GetTitleBlock(), &aBoard.GetProperties() );
    drawingSheet.SetSheetName( std::string() );
    drawingSheet.SetSheetPath( std::string() );
    drawingSheet.SetIsFirstPage( true );
    drawingSheet.SetFileName( utf8( aBoard.GetFileName() ) );
    drawingSheet.SetVariantName( utf8( aBoard.GetCurrentVariant() ) );
    drawingSheet.SetVariantDesc( utf8( aBoard.GetVariantDescription( aBoard.GetCurrentVariant() ) ) );

    std::shared_ptr<DRC_ENGINE> engine = aBoard.GetDesignSettings().m_DRCEngine;
    std::vector<PCB_MARKER*>    markers;
    engine->SetDrawingSheet( &drawingSheet );
    engine->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                 const std::function<void( PCB_MARKER* )>& aPathGenerator )
            {
                markers.push_back( new PCB_MARKER( aItem, aPos, aLayer ) );
                aPathGenerator( markers.back() );
            } );
    aBoard.RecordDRCExclusions();
    aBoard.DeleteMARKERs( true, true );
    engine->RunTests( EDA_UNITS::MM, false, false );
    engine->ClearViolationHandler();
    engine->SetDrawingSheet( nullptr );

    for( PCB_MARKER* marker : markers )
        aBoard.Add( marker );

    aBoard.ResolveDRCExclusions( false );

    auto violations = std::make_shared<DRC_ITEMS_PROVIDER>( &aBoard, MARKER_BASE::MARKER_DRC,
                                                            MARKER_BASE::MARKER_DRAWING_SHEET );
    auto unconnected = std::make_shared<DRC_ITEMS_PROVIDER>( &aBoard, MARKER_BASE::MARKER_RATSNEST );
    auto parity = std::make_shared<DRC_ITEMS_PROVIDER>( &aBoard, MARKER_BASE::MARKER_PARITY );

    for( const auto& provider : { violations, unconnected, parity } )
        provider->SetSeverities( RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING );

    TEMP_FILE report( ".json" );

    if( !DRC_REPORT( &aBoard, EDA_UNITS::MM, violations, unconnected, parity )
                 .WriteJsonReport( wxString::FromUTF8( report.path.string() ) ) )
        throw std::runtime_error( "Cannot write the DRC report" );

    return report.read();
}

// aClose: a failure may leave the open board half changed, so it is closed (not for the
// router's start, move and fix, which leave the board alone, or a collision would end the route).
template <class F>
char* invoke( F&& aOperation, char* aError, size_t aCapacity, bool aClose = true )
{
    std::string message;

    try
    {
        std::string text = aOperation();
        char*       owned = static_cast<char*>( std::malloc( text.size() + 1 ) );

        if( owned )
        {
            std::memcpy( owned, text.c_str(), text.size() + 1 );
            return owned;
        }

        message = "Out of memory";
    }
    catch( const IO_ERROR& error )
    {
        message = utf8( error.What() );
    }
    catch( const std::exception& error )
    {
        message = error.what();
    }

    if( aClose )
        closeBoard();

    if( aError && aCapacity )
        std::snprintf( aError, aCapacity, "%s", message.c_str() );

    return nullptr;
}
// PCB_BASE_FRAME::loadFootprint (load_select_footprint.cpp), without the frame: the project's
// footprint libraries, nets cleared, the board's default text and shape styles applied.
FOOTPRINT* loadFootprint( BOARD& aBoard, const LIB_ID& aFootprintId )
{
    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( aBoard.GetProject() );
    FOOTPRINT*                 footprint = nullptr;
    const wxString             nickname = aFootprintId.GetLibNickname();

    // Libraries load on demand here (see drc()), as cvpcb's testFootprintLink does.
    if( !nickname.IsEmpty() && !adapter->GetLibraryStatus( nickname ) )
        adapter->LoadOne( nickname );

    try
    {
        footprint = adapter->LoadFootprintWithOptionalNickname( aFootprintId, false );
    }
    catch( const IO_ERROR& )
    {
    }

    if( footprint )
    {
        footprint->ClearAllNets();
        BOARD_DESIGN_SETTINGS& bds = aBoard.GetDesignSettings();
        footprint->ApplyDefaultSettings( aBoard, bds.m_StyleFPFields, bds.m_StyleFPText, bds.m_StyleFPShapes,
                                         bds.m_StyleFPDimensions, bds.m_StyleFPBarcodes );
    }

    return footprint;
}

// The open board's file text after an edit; openBoard keeps the board when the app writes it.
std::string saveText( BOARD& aBoard )
{
    TEMP_FILE saved( ".kicad_pcb" );
    wxString  name = wxString::FromUTF8( saved.path.string() ); // SaveBoard takes a non-const reference

    if( !SaveBoard( name, &aBoard, true ) )
        throw std::runtime_error( "Cannot save the board" );

    g_open.returned = saved.read();
    return g_open.returned;
}

// cvpcb/cvpcb.cpp's CV::testFootprintLink (KiCad 10.0.6), unchanged. ERC's footprint-link
// check asks the CvPcb module for it through KIFACE_TEST_FOOTPRINT_LINK; the app links the
// footprint libraries' adapter but not CvPcb.
int testFootprintLink( const wxString& aFootprint, PROJECT* aProject )
{
    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( aProject );
    LIB_ID                  fpID;

    fpID.Parse( aFootprint );

    wxString libName = fpID.GetLibNickname();
    wxString fpName = fpID.GetLibItemName();

    // TODO(JE) this is a bit of a hack; CLI lazy-loading of libraries should be more unified
    // Libraries may not be present at this point if running in kicad-cli because nothings will
    // have triggered a load up to this point
    if( !adapter->GetLibraryStatus( libName ) )
        adapter->LoadOne( libName );

    if( !adapter->HasLibrary( libName, false ) )
        return KIFACE_TEST_FOOTPRINT_LINK_NO_LIBRARY;
    else if( !adapter->HasLibrary( libName, true ) )
        return KIFACE_TEST_FOOTPRINT_LINK_LIBRARY_NOT_ENABLED;
    else if( !adapter->FootprintExists( libName, fpName ) )
        return KIFACE_TEST_FOOTPRINT_LINK_NO_FOOTPRINT;

    return 0;
}

// The CvPcb module as ERC sees it: only the footprint link test.
struct FOOTPRINT_LINK_KIFACE : public HEADLESS_KIFACE
{
    FOOTPRINT_LINK_KIFACE() : HEADLESS_KIFACE( "cvpcb", KIWAY::FACE_CVPCB ) {}
    void* IfaceOrAddress( int aDataId ) override
    {
        return aDataId == KIFACE_TEST_FOOTPRINT_LINK ? (void*) testFootprintLink : nullptr;
    }
};

// The board editor's router interface (PNS_KICAD_IFACE, pcbnew/router/pns_kicad_iface_view.cpp
// after patch 0010) without the editor: the commit code is PNS_KICAD_IFACE's (KiCad 10.0.6),
// without groups and the selection tool, and the preview items it would draw into the view are
// collected as text for the app instead.
class HEADLESS_ROUTER_IFACE : public PNS_KICAD_IFACE_BASE
{
public:
    // The router's preview since the last EraseView, one item per line (cc_board_native_route_move).
    std::ostringstream m_preview;

    HEADLESS_ROUTER_IFACE() { m_preview.imbue( std::locale::classic() ); }

    void EraseView() override
    {
        m_preview.str( std::string() );
        m_preview.clear();
    }

    void DisplayItem( const PNS::ITEM* aItem, int aClearance, bool aEdit, int aFlags ) override
    {
        const auto mm = []( int aValue ) { return pcbIUScale.IUTomm( aValue ); };
        SHAPE_LINE_CHAIN chain;
        int              width = 0;

        switch( aItem->Kind() )
        {
        case PNS::ITEM::LINE_T:
            chain = static_cast<const PNS::LINE*>( aItem )->CLine();
            width = static_cast<const PNS::LINE*>( aItem )->Width();
            break;

        case PNS::ITEM::SEGMENT_T:
        {
            const SEG& seg = static_cast<const PNS::SEGMENT*>( aItem )->Seg();
            chain.Append( seg.A );
            chain.Append( seg.B );
            width = static_cast<const PNS::SEGMENT*>( aItem )->Width();
            break;
        }

        case PNS::ITEM::ARC_T:
            chain = static_cast<const SHAPE_ARC*>( aItem->Shape( -1 ) )->ConvertToPolyline( pcbIUScale.mmToIU( 0.005 ) );
            width = static_cast<const PNS::ARC*>( aItem )->Width();
            break;

        case PNS::ITEM::VIA_T:
        {
            const PNS::VIA* via = static_cast<const PNS::VIA*>( aItem );
            m_preview << "via " << mm( via->Pos().x ) << ' ' << mm( via->Pos().y ) << ' '
                      << mm( via->Diameter( via->Layers().Start() ) ) << '\n';
            return;
        }

        default:
            return; // pads of a dragged footprint: not routed here
        }

        if( chain.PointCount() < 2 )
            return;

        m_preview << "track " << utf8( LSET::Name( GetBoardLayerFromPNSLayer( aItem->Layers().Start() ) ) ) << ' '
                  << mm( width );

        for( const VECTOR2I& point : chain.CPoints() )
            m_preview << ' ' << mm( point.x ) << ' ' << mm( point.y );

        m_preview << '\n';
    }

    // A board item the preview replaces (a shoved track or via).
    void HideItem( PNS::ITEM* aItem ) override
    {
        if( aItem->Parent() )
            m_preview << "hide " << utf8( aItem->Parent()->m_Uuid.AsString() ) << '\n';
    }

    int GetNetCode( PNS::NET_HANDLE aNet ) const override
    {
        return aNet ? static_cast<NETINFO_ITEM*>( aNet )->GetNetCode() : -1;
    }

    wxString GetNetName( PNS::NET_HANDLE aNet ) const override
    {
        return aNet ? static_cast<NETINFO_ITEM*>( aNet )->GetNetname() : wxString();
    }

    void RemoveItem( PNS::ITEM* aItem ) override
    {
        BOARD_ITEM* parent = aItem->Parent();

        if( aItem->OfKind( PNS::ITEM::SOLID_T ) && parent->Type() == PCB_PAD_T )
        {
            m_fpOffsets[static_cast<PAD*>( parent )].p_old = static_cast<PNS::SOLID*>( aItem )->Pos();
            return;
        }

        if( parent )
            m_commit->Remove( parent );
    }

    void UpdateItem( PNS::ITEM* aItem ) override
    {
        BOARD_ITEM* board_item = aItem->Parent();

        switch( aItem->Kind() )
        {
        case PNS::ITEM::ARC_T:
        {
            PNS::ARC*        arc = static_cast<PNS::ARC*>( aItem );
            PCB_ARC*         arc_board = static_cast<PCB_ARC*>( board_item );
            const SHAPE_ARC* arc_shape = static_cast<const SHAPE_ARC*>( arc->Shape( -1 ) );

            m_commit->Modify( arc_board );
            arc_board->SetStart( VECTOR2I( arc_shape->GetP0() ) );
            arc_board->SetEnd( VECTOR2I( arc_shape->GetP1() ) );
            arc_board->SetMid( VECTOR2I( arc_shape->GetArcMid() ) );
            arc_board->SetWidth( arc->Width() );
            break;
        }

        case PNS::ITEM::SEGMENT_T:
        {
            PNS::SEGMENT* seg = static_cast<PNS::SEGMENT*>( aItem );
            PCB_TRACK*    track = static_cast<PCB_TRACK*>( board_item );
            const SEG&    s = seg->Seg();

            m_commit->Modify( track );
            track->SetStart( VECTOR2I( s.A.x, s.A.y ) );
            track->SetEnd( VECTOR2I( s.B.x, s.B.y ) );
            track->SetWidth( seg->Width() );
            break;
        }

        case PNS::ITEM::VIA_T:
        {
            PCB_VIA* via_board = static_cast<PCB_VIA*>( board_item );
            m_commit->Modify( via_board );
            setVia( via_board, static_cast<PNS::VIA*>( aItem ) );
            via_board->SetNet( static_cast<NETINFO_ITEM*>( aItem->Net() ) );
            break;
        }

        case PNS::ITEM::SOLID_T:
            if( aItem->Parent()->Type() == PCB_PAD_T )
            {
                PAD* pad = static_cast<PAD*>( aItem->Parent() );
                m_fpOffsets[pad].p_old = pad->GetPosition();
                m_fpOffsets[pad].p_new = static_cast<PNS::SOLID*>( aItem )->Pos();
            }
            break;

        default:
            m_commit->Modify( aItem->Parent() );
            break;
        }
    }

    void AddItem( PNS::ITEM* aItem ) override
    {
        NETINFO_ITEM*         net = static_cast<NETINFO_ITEM*>( aItem->Net() );
        BOARD_CONNECTED_ITEM* item = nullptr;

        if( !net )
            net = NETINFO_LIST::OrphanedItem();

        switch( aItem->Kind() )
        {
        case PNS::ITEM::ARC_T:
        {
            PNS::ARC* arc = static_cast<PNS::ARC*>( aItem );
            PCB_ARC*  new_arc = new PCB_ARC( m_board, static_cast<const SHAPE_ARC*>( arc->Shape( -1 ) ) );
            new_arc->SetWidth( arc->Width() );
            new_arc->SetLayer( GetBoardLayerFromPNSLayer( arc->Layers().Start() ) );
            new_arc->SetNet( net );
            copyMask( new_arc, aItem );
            item = new_arc;
            break;
        }

        case PNS::ITEM::SEGMENT_T:
        {
            PNS::SEGMENT* seg = static_cast<PNS::SEGMENT*>( aItem );
            PCB_TRACK*    track = new PCB_TRACK( m_board );
            track->SetStart( seg->Seg().A );
            track->SetEnd( seg->Seg().B );
            track->SetWidth( seg->Width() );
            track->SetLayer( GetBoardLayerFromPNSLayer( seg->Layers().Start() ) );
            track->SetNet( net );
            copyMask( track, aItem );
            item = track;
            break;
        }

        case PNS::ITEM::VIA_T:
        {
            PCB_VIA* via_board = new PCB_VIA( m_board );
            via_board->SetNet( net );
            setVia( via_board, static_cast<PNS::VIA*>( aItem ) );

            if( aItem->GetSourceItem() && aItem->GetSourceItem()->Type() == PCB_VIA_T )
            {
                PCB_VIA* sourceVia = static_cast<PCB_VIA*>( aItem->GetSourceItem() );
                via_board->SetFrontTentingMode( sourceVia->GetFrontTentingMode() );
                via_board->SetBackTentingMode( sourceVia->GetBackTentingMode() );
            }

            item = via_board;
            break;
        }

        case PNS::ITEM::SOLID_T:
            m_fpOffsets[static_cast<PAD*>( aItem->Parent() )].p_new = static_cast<PNS::SOLID*>( aItem )->Pos();
            return;

        default:
            return;
        }

        if( net->GetNetCode() <= 0 )
        {
            NETINFO_ITEM* newNetInfo = item->GetNet();
            newNetInfo->SetParent( m_board );
            newNetInfo->SetNetClass( m_board->GetDesignSettings().m_NetSettings->GetDefaultNetclass() );
        }

        if( aItem->IsLocked() )
            item->SetLocked( true );

        aItem->SetParent( item );
        item->ClearFlags();
        m_commit->Add( item );
    }

    // PNS_KICAD_IFACE::Commit without the selection tool's groups.
    void Commit() override
    {
        std::set<FOOTPRINT*> processedFootprints;

        EraseView();

        for( const auto& [pad, fpOffset] : m_fpOffsets )
        {
            FOOTPRINT* footprint = pad->GetParentFootprint();

            if( !processedFootprints.insert( footprint ).second )
                continue;

            m_commit->Modify( footprint );
            footprint->SetPosition( footprint->GetPosition() + fpOffset.p_new - fpOffset.p_old );
        }

        m_fpOffsets.clear();
        m_commit->Push( wxEmptyString );
        m_commit = std::make_unique<BOARD_COMMIT>( static_cast<TOOL_MANAGER*>( nullptr ) );
    }

private:
    // PNS_KICAD_IFACE::createBoardItem and modifyBoardItem's via fields (the net is set apart).
    void setVia( PCB_VIA* aBoardVia, PNS::VIA* aVia )
    {
        aBoardVia->SetPosition( VECTOR2I( aVia->Pos().x, aVia->Pos().y ) );
        aBoardVia->SetWidth( PADSTACK::ALL_LAYERS, aVia->Diameter( 0 ) );
        aBoardVia->SetDrill( aVia->Drill() );
        aBoardVia->SetViaType( aVia->ViaType() ); // MUST be before SetLayerPair()
        aBoardVia->Padstack().SetUnconnectedLayerMode( aVia->UnconnectedLayerMode() );
        aBoardVia->SetIsFree( aVia->IsFree() );
        aBoardVia->SetLayerPair( GetBoardLayerFromPNSLayer( aVia->Layers().Start() ),
                                 GetBoardLayerFromPNSLayer( aVia->Layers().End() ) );

        PNS_LAYER_RANGE holeLayers = aVia->HoleLayers();

        if( holeLayers.Start() >= 0 && holeLayers.End() >= 0 )
        {
            aBoardVia->SetPrimaryDrillStartLayer( GetBoardLayerFromPNSLayer( holeLayers.Start() ) );
            aBoardVia->SetPrimaryDrillEndLayer( GetBoardLayerFromPNSLayer( holeLayers.End() ) );
        }

        aBoardVia->SetFrontPostMachining( aVia->HolePostMachining() );
        aBoardVia->SetSecondaryDrillSize( aVia->SecondaryDrill() );

        if( std::optional<PNS_LAYER_RANGE> secondaryLayers = aVia->SecondaryHoleLayers() )
        {
            aBoardVia->SetSecondaryDrillStartLayer( GetBoardLayerFromPNSLayer( secondaryLayers->Start() ) );
            aBoardVia->SetSecondaryDrillEndLayer( GetBoardLayerFromPNSLayer( secondaryLayers->End() ) );
        }
        else
        {
            aBoardVia->SetSecondaryDrillStartLayer( UNDEFINED_LAYER );
            aBoardVia->SetSecondaryDrillEndLayer( UNDEFINED_LAYER );
        }
    }

    static void copyMask( PCB_TRACK* aTrack, PNS::ITEM* aItem )
    {
        if( aItem->GetSourceItem() && aItem->GetSourceItem()->IsType( { PCB_TRACE_T, PCB_ARC_T } ) )
        {
            PCB_TRACK* sourceTrack = static_cast<PCB_TRACK*>( aItem->GetSourceItem() );
            aTrack->SetHasSolderMask( sourceTrack->HasSolderMask() );
            aTrack->SetLocalSolderMaskMargin( sourceTrack->GetLocalSolderMaskMargin() );
        }
    }

    struct OFFSET
    {
        VECTOR2I p_old, p_new;
    };

    std::map<PAD*, OFFSET>        m_fpOffsets;
    std::unique_ptr<BOARD_COMMIT> m_commit =
            std::make_unique<BOARD_COMMIT>( static_cast<TOOL_MANAGER*>( nullptr ) );
};

// One route at a time on the open board, as ROUTER_TOOL drives the router (TOOL_BASE::Reset,
// prepareInteractive, performRouting). Never destroyed, like the open board.
struct CORV_ROUTE
{
    std::unique_ptr<HEADLESS_ROUTER_IFACE> iface;
    std::unique_ptr<PNS::ROUTER>           router; // freed before iface (NODE needs the resolver)
    PNS::ITEM*                             startItem = nullptr;
    bool                                   dragging = false; // a drag, not a placement
    bool                                   fixed = false;    // a fix already committed (drag, tuning)
    int                                    kind = 0;         // cc_board_native_route_start's kind
};

CORV_ROUTE& g_route = *new CORV_ROUTE;

void endRoute()
{
    if( g_route.router && g_route.router->RoutingInProgress() )
        g_route.router->StopRouting();

    g_route.router.reset();
    g_route.iface.reset();
    g_route.startItem = nullptr;
    g_route.dragging = false;
    g_route.fixed = false;
    g_route.kind = 0;
}

PNS::ROUTER& router()
{
    if( !g_route.router || !g_route.router->RoutingInProgress() )
        throw std::runtime_error( "No route in progress" );

    return *g_route.router;
}

// PNS::ROUTING_SETTINGS has its defaults without a parent (KiCad's are in pcbnew.json).
PNS::ROUTING_SETTINGS& routingSettings()
{
    static PNS::ROUTING_SETTINGS* settings = new PNS::ROUTING_SETTINGS( nullptr, "tools.pns" );
    return *settings;
}

VECTOR2I toIU( double aX, double aY ) { return VECTOR2I( pcbIUScale.mmToIU( aX ), pcbIUScale.mmToIU( aY ) ); }

// TOOL_BASE::pickSingleItem: the routable copper item at (or within 1 mm of) aP, of net aNet
// when one is given; pads and vias first, then track ends, those on aLayer first. aTracks:
// tracks only (length tuning), or tracks and vias (dragging).
enum class PICK { ANY, TRACKS, TRACKS_AND_VIAS };

PNS::ITEM* pickItem( const VECTOR2I& aP, PNS::NET_HANDLE aNet, int aLayer, PNS::ITEM* aAvoid = nullptr,
                     PICK aPick = PICK::ANY )
{
    PNS::ITEM*  best = nullptr;
    int         bestRank = 4;
    SEG::ecoord bestDistance = VECTOR2I::ECOORD_MAX;

    for( int slop : { 0, pcbIUScale.mmToIU( 1.0 ) } )
    {
        const PNS::ITEM_SET candidates = g_route.router->QueryHoverItems( aP, slop );

        for( PNS::ITEM* item : candidates.CItems() )
        {
            if( item == aAvoid || !item->IsRoutable() || !g_route.iface->IsPNSCopperLayer( item->Layers().Start() ) )
                continue;

            if( aNet && g_route.iface->GetNetCode( aNet ) > 0 && item->Net() != aNet )
                continue;

            if( ( aPick == PICK::TRACKS && !item->OfKind( PNS::ITEM::SEGMENT_T | PNS::ITEM::ARC_T ) )
                || ( aPick == PICK::TRACKS_AND_VIAS
                     && !item->OfKind( PNS::ITEM::SEGMENT_T | PNS::ITEM::ARC_T | PNS::ITEM::VIA_T ) ) )
            {
                continue;
            }

            const bool  onLayer = item->Layers().Overlaps( aLayer );
            int         rank;
            SEG::ecoord distance;

            if( item->OfKind( PNS::ITEM::VIA_T | PNS::ITEM::SOLID_T ) )
            {
                rank = onLayer ? 0 : 2;
                distance = ( item->Shape( aLayer )->Centre() - aP ).SquaredEuclideanNorm();
            }
            else if( item->OfKind( PNS::ITEM::SEGMENT_T | PNS::ITEM::ARC_T ) )
            {
                PNS::LINKED_ITEM* linked = static_cast<PNS::LINKED_ITEM*>( item );
                rank = onLayer ? 1 : 3;
                distance = std::min( ( linked->Anchor( 0 ) - aP ).SquaredEuclideanNorm(),
                                     ( linked->Anchor( 1 ) - aP ).SquaredEuclideanNorm() );
            }
            else
            {
                continue;
            }

            if( rank < bestRank || ( rank == bestRank && distance < bestDistance ) )
            {
                best = item;
                bestRank = rank;
                bestDistance = distance;
            }
        }

        if( best )
            break;
    }

    return best;
}

// TOOL_BASE::snapToItem without the grid (the app snaps to its grid): a pad's nearest anchor,
// a via's centre, a track's end within half its width or else the nearest point on it.
VECTOR2I snapToItem( PNS::ITEM* aItem, const VECTOR2I& aP )
{
    if( !aItem )
        return aP;

    switch( aItem->Kind() )
    {
    case PNS::ITEM::SOLID_T:
    {
        PNS::SOLID* solid = static_cast<PNS::SOLID*>( aItem );

        if( solid->AnchorPoints().empty() )
            return solid->Anchor( 0 );

        return *std::min_element( solid->AnchorPoints().begin(), solid->AnchorPoints().end(),
                                  [&]( const VECTOR2I& a, const VECTOR2I& b )
                                  {
                                      return ( a - aP ).SquaredEuclideanNorm() < ( b - aP ).SquaredEuclideanNorm();
                                  } );
    }

    case PNS::ITEM::VIA_T:
        return static_cast<PNS::VIA*>( aItem )->Pos();

    case PNS::ITEM::SEGMENT_T:
    case PNS::ITEM::ARC_T:
    {
        PNS::LINKED_ITEM* linked = static_cast<PNS::LINKED_ITEM*>( aItem );
        const VECTOR2I    a = linked->Anchor( 0 ), b = linked->Anchor( 1 );
        const SEG::ecoord halfWidth = SEG::Square( linked->Width() / 2 );
        const SEG::ecoord da = ( aP - a ).SquaredEuclideanNorm(), db = ( aP - b ).SquaredEuclideanNorm();

        if( da < halfWidth || db < halfWidth )
            return da < db ? a : b;

        if( aItem->Kind() == PNS::ITEM::SEGMENT_T )
            return static_cast<PNS::SEGMENT*>( aItem )->Seg().NearestPoint( aP );

        return aP;
    }

    default:
        return aP;
    }
}

// The end item under aP for the route in progress: same net, not the start item.
PNS::ITEM* endItem( const VECTOR2I& aP )
{
    const std::vector<PNS::NET_HANDLE> nets = g_route.router->GetCurrentNets();
    return pickItem( aP, nets.empty() ? nullptr : nets.front(), g_route.router->GetCurrentLayer(),
                     g_route.startItem );
}
// The layers kicad-cli plots with the board's plot settings (--board-plot-params): the
// selected non-copper layers and every enabled copper layer, in the editor's layer order.
LSEQ plottedLayers( BOARD& aBoard, const PCB_PLOT_PARAMS& aOpts )
{
    LSET layers = ( aOpts.GetLayerSelection() & LSET::AllNonCuMask() ) | ( aBoard.GetEnabledLayers() & LSET::AllCuMask() );
    return LSET( layers & aBoard.GetEnabledLayers() ).UIOrder();
}

// Board Setup as text, tab-separated, millimetres:
//   layers <copper layer count>
//   rule <name> <value>              for each entry of kRules
//   class <name> <clearance> <track> <via diameter> <via drill> <pair width> <pair gap>
//   assign <pattern> <netclass>      a netclass pattern assignment
struct RULE
{
    const char* name;
    int BOARD_DESIGN_SETTINGS::*field;
};

const RULE kRules[] = {
    { "min_clearance", &BOARD_DESIGN_SETTINGS::m_MinClearance },
    { "min_track_width", &BOARD_DESIGN_SETTINGS::m_TrackMinWidth },
    { "min_annular_width", &BOARD_DESIGN_SETTINGS::m_ViasMinAnnularWidth },
    { "min_via_diameter", &BOARD_DESIGN_SETTINGS::m_ViasMinSize },
    { "min_through_hole", &BOARD_DESIGN_SETTINGS::m_MinThroughDrill },
    { "copper_edge_clearance", &BOARD_DESIGN_SETTINGS::m_CopperEdgeClearance },
    { "hole_clearance", &BOARD_DESIGN_SETTINGS::m_HoleClearance },
    { "hole_to_hole", &BOARD_DESIGN_SETTINGS::m_HoleToHoleMin },
};

std::vector<std::string> splitTabs( const std::string& aLine )
{
    std::vector<std::string> fields;
    std::istringstream       in( aLine );
    std::string              field;

    while( std::getline( in, field, '\t' ) )
        fields.push_back( field );

    return fields;
}

int toIU( const std::string& aMillimetres )
{
    std::istringstream in( aMillimetres );
    in.imbue( std::locale::classic() );
    double value = 0;

    if( !( in >> value ) || value < 0 || value > 1000 )
        throw std::runtime_error( "Not a size in millimetres: " + aMillimetres );

    return pcbIUScale.mmToIU( value );
}

std::string boardSetup( BOARD& aBoard )
{
    BOARD_DESIGN_SETTINGS&        bds = aBoard.GetDesignSettings();
    std::shared_ptr<NET_SETTINGS> nets = bds.m_NetSettings;
    std::ostringstream            out;
    out.imbue( std::locale::classic() );
    out << "layers\t" << aBoard.GetCopperLayerCount() << '\n';

    for( const RULE& rule : kRules )
        out << "rule\t" << rule.name << '\t' << pcbIUScale.IUTomm( bds.*rule.field ) << '\n';

    auto line = [&]( const NETCLASS& aClass )
    {
        out << "class\t" << utf8( aClass.GetName() );

        for( int value : { aClass.GetClearance(), aClass.GetTrackWidth(), aClass.GetViaDiameter(), aClass.GetViaDrill(),
                           aClass.GetDiffPairWidth(), aClass.GetDiffPairGap() } )
            out << '\t' << ( value < 0 ? 0.0 : pcbIUScale.IUTomm( value ) );

        out << '\n';
    };

    line( *nets->GetDefaultNetclass() );

    for( const auto& [name, netclass] : nets->GetNetclasses() )
        line( *netclass );

    for( const auto& [matcher, netclass] : nets->GetNetclassPatternAssignments() )
        out << "assign\t" << utf8( matcher->GetPattern() ) << '\t' << utf8( netclass ) << '\n';

    // The physical stack-up's dielectric layers in board order (PANEL_SETUP_BOARD_STACKUP):
    // index, kind (core or prepreg), thickness, material, relative permittivity. A board
    // without a stack-up reports KiCad's default for its copper layer count.
    BOARD_STACKUP stackup = bds.GetStackupDescriptor();

    if( stackup.GetList().empty() )
        stackup.BuildDefaultStackupList( &bds, aBoard.GetCopperLayerCount() );

    int index = 0;

    for( BOARD_STACKUP_ITEM* item : stackup.GetList() )
    {
        if( item->GetType() == BS_ITEM_TYPE_DIELECTRIC )
            out << "dielectric\t" << index++ << '\t' << utf8( item->GetTypeName() ) << '\t'
                << pcbIUScale.IUTomm( item->GetThickness() ) << '\t' << utf8( item->GetMaterial() ) << '\t'
                << item->GetEpsilonR() << '\n';
    }

    return out.str();
}

// PANEL_SETUP_LAYERS and PANEL_SETUP_RULES/NETCLASSES' transfer to the board, from the text
// above. Every class and assignment is replaced; the Default class keeps its other settings.
void applyBoardSetup( BOARD& aBoard, const std::string& aSetup )
{
    BOARD_DESIGN_SETTINGS&        bds = aBoard.GetDesignSettings();
    std::shared_ptr<NET_SETTINGS> nets = bds.m_NetSettings;
    std::map<wxString, std::shared_ptr<NETCLASS>> classes;
    std::vector<std::pair<wxString, wxString>>    assignments;
    std::vector<std::pair<const RULE*, int>>      rules;
    std::optional<std::array<int, 6>>             defaults;
    // Dielectric index, thickness, material and relative permittivity.
    std::vector<std::tuple<int, int, wxString, double>> dielectrics;
    std::istringstream in( aSetup );
    std::string        text;
    int                layers = aBoard.GetCopperLayerCount();

    while( std::getline( in, text ) )
    {
        std::vector<std::string> f = splitTabs( text );

        if( f.empty() || f[0].empty() )
            continue;

        if( f[0] == "layers" && f.size() == 2 )
        {
            layers = std::atoi( f[1].c_str() );

            if( layers < 2 || layers > 32 || layers % 2 )
                throw std::runtime_error( "Copper layers must be an even count from 2 to 32" );
        }
        else if( f[0] == "rule" && f.size() == 3 )
        {
            const RULE* rule = std::find_if( std::begin( kRules ), std::end( kRules ),
                                             [&]( const RULE& r ) { return f[1] == r.name; } );

            if( rule == std::end( kRules ) )
                throw std::runtime_error( "Unknown design rule " + f[1] );

            rules.emplace_back( rule, toIU( f[2] ) );
        }
        else if( f[0] == "class" && f.size() == 8 )
        {
            const wxString     name = wxString::FromUTF8( f[1] );
            std::array<int, 6> sizes;

            if( name.IsEmpty() )
                throw std::runtime_error( "A netclass needs a name" );

            for( size_t i = 0; i < sizes.size(); ++i )
                sizes[i] = toIU( f[i + 2] );

            if( name == NETCLASS::Default )
            {
                defaults = sizes;
                continue;
            }

            auto netclass = std::make_shared<NETCLASS>( name );
            netclass->SetClearance( sizes[0] );
            netclass->SetTrackWidth( sizes[1] );
            netclass->SetViaDiameter( sizes[2] );
            netclass->SetViaDrill( sizes[3] );
            netclass->SetDiffPairWidth( sizes[4] );
            netclass->SetDiffPairGap( sizes[5] );
            classes[name] = netclass;
        }
        else if( f[0] == "assign" && f.size() == 3 )
        {
            assignments.emplace_back( wxString::FromUTF8( f[1] ), wxString::FromUTF8( f[2] ) );
        }
        else if( f[0] == "dielectric" && f.size() == 6 )
        {
            std::istringstream er( f[5] );
            er.imbue( std::locale::classic() );
            double epsilon = 0;

            if( !( er >> epsilon ) || epsilon < 1 || epsilon > 100 )
                throw std::runtime_error( "A dielectric's relative permittivity is 1 to 100" );

            const int thickness = toIU( f[3] );

            if( thickness <= 0 )
                throw std::runtime_error( "A dielectric layer needs a thickness" );

            dielectrics.emplace_back( std::atoi( f[1].c_str() ), thickness, wxString::FromUTF8( f[4] ), epsilon );
        }
        else
        {
            throw std::runtime_error( "Unreadable Board Setup line: " + text );
        }
    }

    for( const auto& [pattern, netclass] : assignments )
    {
        if( netclass != NETCLASS::Default && !classes.count( netclass ) )
            throw std::runtime_error( "No netclass " + utf8( netclass ) + " for " + utf8( pattern ) );
    }

    // Removing copper layers: refused while tracks, vias or zones use them (the board editor
    // asks to delete those items first).
    if( layers < aBoard.GetCopperLayerCount() )
    {
        LSET removed = aBoard.GetEnabledLayers() & LSET::AllCuMask() & ~LSET::AllCuMask( layers );

        for( PCB_TRACK* track : aBoard.Tracks() )
        {
            if( track->Type() != PCB_VIA_T && removed.test( track->GetLayer() ) )
                throw std::runtime_error( "Remove the tracks on " + utf8( aBoard.GetLayerName( track->GetLayer() ) ) + " first" );
        }

        for( ZONE* zone : aBoard.Zones() )
        {
            if( ( zone->GetLayerSet() & removed ).any() )
                throw std::runtime_error( "Remove the zones on the removed copper layers first" );
        }
    }

    // Everything checked: apply.
    for( const auto& [rule, value] : rules )
        bds.*rule->field = value;

    if( defaults )
    {
        std::shared_ptr<NETCLASS> netclass = nets->GetDefaultNetclass();
        netclass->SetClearance( ( *defaults )[0] );
        netclass->SetTrackWidth( ( *defaults )[1] );
        netclass->SetViaDiameter( ( *defaults )[2] );
        netclass->SetViaDrill( ( *defaults )[3] );
        netclass->SetDiffPairWidth( ( *defaults )[4] );
        netclass->SetDiffPairGap( ( *defaults )[5] );
    }

    nets->SetNetclasses( classes );
    nets->ClearNetclassPatternAssignments();

    for( const auto& [pattern, netclass] : assignments )
        nets->SetNetclassPatternAssignment( pattern, netclass );

    nets->ClearAllCaches();

    if( layers != aBoard.GetCopperLayerCount() )
    {
        aBoard.SetCopperLayerCount( layers );
        bds.GetStackupDescriptor().SynchronizeWithBoard( &bds );
        dielectrics.clear(); // they described the old layer count: apply them in a second step
    }

    // PANEL_SETUP_BOARD_STACKUP's transfer: the dielectrics by index, the board's thickness from
    // the stack-up. Only a real change makes the stack-up the board's own (m_HasStackup).
    if( !dielectrics.empty() )
    {
        BOARD_STACKUP& stackup = bds.GetStackupDescriptor();

        if( stackup.GetList().empty() )
            stackup.BuildDefaultStackupList( &bds, aBoard.GetCopperLayerCount() );

        std::vector<BOARD_STACKUP_ITEM*> items;

        for( BOARD_STACKUP_ITEM* item : stackup.GetList() )
        {
            if( item->GetType() == BS_ITEM_TYPE_DIELECTRIC )
                items.push_back( item );
        }

        bool changed = false;

        for( const auto& [i, thickness, material, epsilon] : dielectrics )
        {
            if( i < 0 || i >= static_cast<int>( items.size() ) )
                throw std::runtime_error( "No dielectric layer " + std::to_string( i ) + " in the stack-up" );

            BOARD_STACKUP_ITEM* item = items[i];

            if( item->GetThickness() != thickness || item->GetMaterial() != material
                || std::abs( item->GetEpsilonR() - epsilon ) > 1e-9 )
            {
                item->SetThickness( thickness );
                item->SetMaterial( material );
                item->SetEpsilonR( epsilon );
                changed = true;
            }
        }

        if( changed )
        {
            bds.m_HasStackup = true;
            bds.SetBoardThickness( stackup.BuildBoardThicknessFromStackup() );
        }
    }

    aBoard.SynchronizeNetsAndNetClasses( false );
    aBoard.BuildConnectivity();
}

// The board's project file text, as SETTINGS_MANAGER::SaveProject would write it.
std::string projectText( BOARD& aBoard )
{
    PROJECT* project = aBoard.GetProject();

    if( !project )
        throw std::runtime_error( "The board has no project" );

    fs::path folder = fs::temp_directory_path() / ( "corvcad-project-" + std::to_string( getpid() ) );
    std::error_code ignored;
    fs::remove_all( folder, ignored );
    fs::create_directories( folder );
    PROJECT_FILE& file = project->GetProjectFile();

    if( !file.SaveToFile( wxString::FromUTF8( folder.string() ), true ) )
    {
        fs::remove_all( folder, ignored );
        throw std::runtime_error( "Cannot write the project settings" );
    }

    std::string text;

    for( const fs::directory_entry& entry : fs::directory_iterator( folder ) )
    {
        if( entry.path().extension() == ".kicad_pro" )
            text = readFile( entry.path() );
    }

    fs::remove_all( folder, ignored );

    if( text.empty() )
        throw std::runtime_error( "Cannot read the project settings" );

    return text;
}
} // namespace

// Overrides kicad_session.cpp's weak default (none), so the session's ERC runs the footprint
// link check when the board code is linked, as kicad-cli's does.
KIFACE* CorvCadFootprintLinkKiface()
{
    start();
    static FOOTPRINT_LINK_KIFACE kiface;
    return &kiface;
}

extern "C" {

const char* cc_board_native_backend( void ) { return "kicad-10.0.6-native"; }

void cc_board_native_reload_libraries( void )
{
    // As KiCad's library table dialog does after a save: the project tables are rebuilt and the
    // cached project libraries dropped, so the next lookup reads the files again.
    if( CorvCadStartKiCad() )
        Pgm().GetLibraryManager().ReloadTables( LIBRARY_TABLE_SCOPE::PROJECT );
}

// Not in the public header: how many times a board was loaded (session/board_test.c).
int corvcad_board_native_loads( void ) { return g_loads; }

char* cc_board_native_drc( const char* aPath, int aRefillZones, char* aError, size_t aCapacity )
{
    return invoke(
            [&]
            {
                BOARD& board = openBoard( aPath );

                if( aRefillZones )
                    fillZones( board );

                std::string report = drc( board );

                if( aRefillZones )
                    closeBoard(); // the refill is not saved: the board no longer matches the file

                return report;
            },
            aError, aCapacity );
}

char* cc_board_native_fill_zones( const char* aPath, char* aError, size_t aCapacity )
{
    return invoke(
            [&]
            {
                BOARD& board = openBoard( aPath );
                fillZones( board );
                return saveText( board );
            },
            aError, aCapacity );
}

// The ratsnest as the board editor draws it: each net's visible unrouted edges after
// RecalculateRatsnest. kicad-cli's DRC reports the same edges as unconnected items.
char* cc_board_native_ratsnest( const char* aPath, char* aError, size_t aCapacity )
{
    return invoke(
            [&]
            {
                BOARD*                             board = &openBoard( aPath );
                std::shared_ptr<CONNECTIVITY_DATA> connectivity = board->GetConnectivity();
                connectivity->RecalculateRatsnest();

                std::ostringstream out;
                out.imbue( std::locale::classic() );

                for( int net = 1; net < connectivity->GetNetCount(); ++net )
                {
                    RN_NET*       ratsnest = connectivity->GetRatsnestForNet( net );
                    NETINFO_ITEM* info = board->FindNet( net );

                    if( !ratsnest || !info )
                        continue;

                    for( const CN_EDGE& edge : ratsnest->GetEdges() )
                    {
                        if( !edge.IsVisible() )
                            continue;

                        const VECTOR2I a = edge.GetSourcePos(), b = edge.GetTargetPos();
                        out << pcbIUScale.IUTomm( a.x ) << ' ' << pcbIUScale.IUTomm( a.y ) << ' '
                            << pcbIUScale.IUTomm( b.x ) << ' ' << pcbIUScale.IUTomm( b.y ) << ' '
                            << utf8( info->GetNetname() ) << '\n';
                    }
                }

                return out.str();
            },
            aError, aCapacity );
}

// The widths the board editor's track and via tools take from each net's effective netclass
// (the Default netclass first, under an empty name).
char* cc_board_native_net_rules( const char* aPath, char* aError, size_t aCapacity )
{
    return invoke(
            [&]
            {
                BOARD*                        board = &openBoard( aPath );
                std::shared_ptr<NET_SETTINGS> settings = board->GetDesignSettings().m_NetSettings;
                std::ostringstream            out;
                out.imbue( std::locale::classic() );

                auto line = [&]( const NETCLASS& aClass, const wxString& aName )
                {
                    out << pcbIUScale.IUTomm( aClass.GetTrackWidth() ) << ' ' << pcbIUScale.IUTomm( aClass.GetViaDiameter() )
                        << ' ' << pcbIUScale.IUTomm( aClass.GetViaDrill() ) << ' '
                        << pcbIUScale.IUTomm( aClass.GetClearance() ) << ' ' << utf8( aName ) << '\n';
                };

                line( *settings->GetDefaultNetclass(), wxEmptyString );

                for( NETINFO_ITEM* net : board->GetNetInfo() )
                {
                    if( net->GetNetCode() > 0 )
                        line( *settings->GetEffectiveNetClass( net->GetNetname() ), net->GetNetname() );
                }

                return out.str();
            },
            aError, aCapacity );
}

// DIALOG_UPDATE_PCB::PerformUpdate without the dialog: re-link footprints by reference (CorvCAD
// boards need not carry symbol paths), keep extra footprints, update fields. The updater
// pushes its own commit (headless/pcbnew/board_commit.cpp).
char* cc_board_native_update_from_netlist( const char* aPath, const char* aNetlist, char* aReport,
                                           size_t aReportCapacity, char* aError, size_t aCapacity )
{
    if( aReport && aReportCapacity )
        *aReport = 0;

    return invoke(
            [&]
            {
                if( !aNetlist )
                    throw std::runtime_error( "Missing netlist" );

                BOARD*                 board = &openBoard( aPath );
                NETLIST                netlist;
                KICAD_NETLIST_READER( new STRING_LINE_READER( std::string( aNetlist ), wxS( "netlist" ) ), &netlist )
                        .LoadNetlist();
                netlist.SetFindByTimeStamp( false );
                netlist.SetReplaceFootprints( true );

                WX_STRING_REPORTER    reporter;
                BOARD_NETLIST_UPDATER updater( nullptr, board );
                updater.SetReporter( &reporter );
                updater.SetFootprintLoader( [&]( const LIB_ID& aId ) { return loadFootprint( *board, aId ); } );
                updater.SetLookupByTimestamp( false );
                updater.SetDeleteUnusedFootprints( false );
                updater.SetReplaceFootprints( true );
                updater.SetUpdateFields( true );
                updater.UpdateNetlist( netlist );

                if( aReport && aReportCapacity )
                    std::snprintf( aReport, aReportCapacity, "%s", utf8( reporter.GetMessages() ).c_str() );

                return saveText( *board );
            },
            aError, aCapacity );
}

// Fabrication outputs as kicad-cli 10.0.6 writes them into aDirectory:
//   pcb export gerbers --board-plot-params [--check-zones]   (PCBNEW_JOBS_HANDLER::JobExportGerbers)
//   pcb export drill --format excellon --drill-origin absolute --excellon-units mm
//                    --excellon-zeros-format decimal --excellon-separate-th   (JobExportDrill)
//   pcb export pos --format csv --units mm --side both -o <board>-pos.csv   (JobExportPos)
// Returns the written file names, one per line.
char* cc_board_native_fab_outputs( const char* aPath, const char* aDirectory, int aRefillZones, char* aError,
                                   size_t aCapacity )
{
    return invoke(
            [&]
            {
                if( !aDirectory )
                    throw std::runtime_error( "Missing output directory" );

                BOARD*   board = &openBoard( aPath );
                wxString outDir = wxString::FromUTF8( aDirectory );
                std::string written;

                if( !wxFileName::Mkdir( outDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
                    throw std::runtime_error( std::string( "Cannot create " ) + aDirectory );

                if( aRefillZones )
                    fillZones( *board );

                // Gerbers with the board's plot settings.
                PCB_PLOT_PARAMS       plotOpts = board->GetPlotOptions();
                GERBER_JOBFILE_WRITER jobfile( board );

                for( PCB_LAYER_ID layer : plottedLayers( *board, plotOpts ) )
                {
                    LSEQ sequence{ layer };

                    for( PCB_LAYER_ID all : plotOpts.GetPlotOnAllLayersSequence() )
                    {
                        if( std::find( sequence.begin(), sequence.end(), all ) == sequence.end() )
                            sequence.push_back( all );
                    }

                    wxFileName fn( board->GetFileName() );
                    wxString   layerName = board->GetLayerName( layer );
                    BuildPlotFileName( &fn, outDir, layerName,
                                       plotOpts.GetUseGerberProtelExtensions() ? GetGerberProtelExtension( layer )
                                                                               : wxString( FILEEXT::GerberFileExtension ) );
                    wxString fullName = fn.GetFullName();
                    jobfile.AddGbrFile( layer, fullName );

                    std::unique_ptr<PLOTTER> plotter( StartPlotBoard( board, &plotOpts, layer, layerName,
                                                                      fn.GetFullPath(), wxEmptyString, wxEmptyString ) );

                    if( !plotter )
                        throw std::runtime_error( "Cannot plot " + utf8( fn.GetFullPath() ) );

                    PlotBoardLayers( board, plotter.get(), sequence, plotOpts );
                    plotter->EndPlot();
                    written += utf8( fullName ) + '\n';
                }

                wxFileName job( board->GetFileName() );
                BuildPlotFileName( &job, outDir, wxT( "job" ), FILEEXT::GerberJobFileExtension );

                if( !jobfile.CreateJobFile( job.GetFullPath() ) )
                    throw std::runtime_error( "Cannot write the Gerber job file" );

                written += utf8( job.GetFullName() ) + '\n';

                // Excellon drill, PTH and NPTH in separate files; DRILL_PRECISION( 3, 3 ) for mm.
                WX_STRING_REPORTER drillReport;
                EXCELLON_WRITER    drill( board );
                drill.SetFormat( true, EXCELLON_WRITER::DECIMAL_FORMAT, 3, 3 );
                drill.SetOptions( false, false, VECTOR2I( 0, 0 ), false );
                drill.SetRouteModeForOvalHoles( false );

                if( !drill.CreateDrillandMapFilesSet( outDir, true, false, &drillReport ) )
                    throw std::runtime_error( "Cannot write the drill files: " + utf8( drillReport.GetMessages() ) );

                // Placement, both sides in one file as kicad-cli writes it (<board>-pos.csv here).
                wxFileName pos( board->GetFileName() );
                pos.SetPath( outDir );
                pos.SetName( pos.GetName() + wxT( "-" ) + FILEEXT::FootprintPlaceFileExtension );
                pos.SetExt( FILEEXT::CsvFileExtension );
                PLACE_FILE_EXPORTER placement( board, true, false, false, false, false, true, true, true, false, false );
                std::ofstream       out( pos.GetFullPath().fn_str(), std::ios::binary );
                out << placement.GenPositionData();

                if( !out )
                    throw std::runtime_error( "Cannot write " + utf8( pos.GetFullPath() ) );

                written += utf8( pos.GetFullName() ) + '\n';

                if( aRefillZones )
                    closeBoard(); // the refill is not saved

                // The drill writer names its own files; list the directory's drill files.
                wxArrayString drills;
                wxDir::GetAllFiles( outDir, &drills, wxString( wxT( "*." ) ) + FILEEXT::DrillFileExtension, wxDIR_FILES );

                for( const wxString& file : drills )
                    written += utf8( wxFileName( file ).GetFullName() ) + '\n';

                return written;
            },
            aError, aCapacity );
}

// The Plot dialog's PDF (one page per layer, one file) or SVG (one file per layer) with the
// board's plot settings and layers, through PCB_PLOTTER as kicad-cli's pdf and svg exports.
char* cc_board_native_plot( const char* aPath, const char* aDirectory, int aFormat, char* aError, size_t aCapacity )
{
    return invoke(
            [&]
            {
                if( !aDirectory )
                    throw std::runtime_error( "Missing output directory" );

                BOARD*   board = &openBoard( aPath );
                wxString outDir = wxString::FromUTF8( aDirectory );

                if( !wxFileName::Mkdir( outDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
                    throw std::runtime_error( std::string( "Cannot create " ) + aDirectory );

                PCB_PLOT_PARAMS plotOpts = board->GetPlotOptions();
                const bool      pdf = aFormat == 0;
                plotOpts.SetFormat( pdf ? PLOT_FORMAT::PDF : PLOT_FORMAT::SVG );
                plotOpts.m_PDFSingle = pdf;
                LSEQ layers = plottedLayers( *board, plotOpts );

                if( layers.empty() )
                    throw std::runtime_error( "The board's plot settings select no layer" );

                wxString output = outDir;

                if( pdf )
                {
                    wxFileName fn( board->GetFileName() );
                    fn.SetPath( outDir );
                    fn.SetExt( GetDefaultPlotExtension( PLOT_FORMAT::PDF ) );
                    output = fn.GetFullPath();
                }

                WX_STRING_REPORTER reporter;
                PCB_PLOTTER        plotter( board, &reporter, plotOpts );

                if( !plotter.Plot( output, layers, plotOpts.GetPlotOnAllLayersSequence(), false, pdf ) )
                    throw std::runtime_error( "Cannot plot: " + utf8( reporter.GetMessages() ) );

                std::string   written;
                wxArrayString files;
                wxDir::GetAllFiles( outDir, &files, pdf ? wxS( "*.pdf" ) : wxS( "*.svg" ), wxDIR_FILES );

                for( const wxString& file : files )
                    written += utf8( wxFileName( file ).GetFullName() ) + '\n';

                return written;
            },
            aError, aCapacity );
}

char* cc_board_native_board_setup( const char* aPath, char* aError, size_t aCapacity )
{
    return invoke( [&] { return boardSetup( openBoard( aPath ) ); }, aError, aCapacity );
}

// The board text, and in aProjectText the project file's (rules and netclasses live there).
// The board stays open only once the app writes both (the project is kept as applied).
char* cc_board_native_apply_board_setup( const char* aPath, const char* aSetup, char** aProjectText, char* aError,
                                         size_t aCapacity )
{
    if( aProjectText )
        *aProjectText = nullptr;

    return invoke(
            [&]
            {
                if( !aSetup || !aProjectText )
                    throw std::runtime_error( "Missing Board Setup" );

                BOARD& board = openBoard( aPath );
                applyBoardSetup( board, aSetup );
                std::string project = projectText( board );
                std::string text = saveText( board );
                *aProjectText = static_cast<char*>( std::malloc( project.size() + 1 ) );

                if( !*aProjectText )
                    throw std::runtime_error( "Out of memory" );

                std::memcpy( *aProjectText, project.c_str(), project.size() + 1 );
                return text;
            },
            aError, aCapacity );
}

// Board edits with KiCad's model on the open board; one tab-separated request, the board's new
// text back:
//   flip <reference>                     EDIT_TOOL::Flip of one footprint: left-right (KiCad's
//                                        default) about its position, to the other side
//   track <uuid> <width mm> <layer>      a track or arc's width and copper layer
//   dimension <layer> <x0> <y0> <x1> <y1> <height>
//                                        DRAWING_TOOL's aligned dimension, with the board's
//                                        dimension and layer defaults
char* cc_board_native_edit( const char* aPath, const char* aRequest, char* aError, size_t aCapacity )
{
    return invoke(
            [&]
            {
                std::vector<std::string> field;
                std::istringstream       request( aRequest ? aRequest : "" );

                for( std::string part; std::getline( request, part, '\t' ); )
                    field.push_back( part );

                auto number = [&]( size_t aIndex )
                {
                    double             value = 0;
                    std::istringstream in( aIndex < field.size() ? field[aIndex] : std::string() );
                    in.imbue( std::locale::classic() );

                    if( !( in >> value ) || !std::isfinite( value ) )
                        throw std::runtime_error( "Expected a number in the board edit" );

                    return value;
                };

                BOARD&                 board = openBoard( aPath );
                BOARD_DESIGN_SETTINGS& bds = board.GetDesignSettings();

                if( field.size() == 2 && field[0] == "flip" )
                {
                    FOOTPRINT* footprint = board.FindFootprintByReference( wxString::FromUTF8( field[1] ) );

                    if( !footprint )
                        throw std::runtime_error( "No footprint " + field[1] );

                    footprint->Flip( footprint->GetPosition(), FLIP_DIRECTION::LEFT_RIGHT );
                }
                else if( field.size() == 4 && field[0] == "track" )
                {
                    const double width = number( 2 );
                    PCB_LAYER_ID layer = board.GetLayerID( wxString::FromUTF8( field[3] ) );
                    PCB_TRACK*   track = nullptr;

                    for( PCB_TRACK* item : board.Tracks() )
                    {
                        if( item->Type() != PCB_VIA_T && item->m_Uuid.AsStdString() == field[1] )
                            track = item;
                    }

                    if( !track )
                        throw std::runtime_error( "No track " + field[1] );

                    if( width < 0.05 || width > 25 )
                        throw std::runtime_error( "Use a track width of 0.05 to 25 mm" );

                    if( !IsCopperLayer( layer ) || !board.IsLayerEnabled( layer ) )
                        throw std::runtime_error( "Choose one of the board's copper layers" );

                    track->SetWidth( toIU( width, 0 ).x );
                    track->SetLayer( layer );
                }
                else if( field.size() == 10 && field[0] == "zone" )
                {
                    // ZONE_SETTINGS' fields of the zone properties dialog; the app refills after.
                    ZONE* zone = nullptr;

                    for( ZONE* item : board.Zones() )
                    {
                        if( item->m_Uuid.AsStdString() == field[1] )
                            zone = item;
                    }

                    if( !zone || !zone->IsOnCopperLayer() )
                        throw std::runtime_error( "No copper zone " + field[1] );

                    if( zone->IsTeardropArea() )
                        throw std::runtime_error( "A teardrop follows the board's teardrop settings, not zone properties" );

                    NETINFO_ITEM* net = field[2].empty() ? nullptr : board.FindNet( wxString::FromUTF8( field[2] ) );

                    if( !field[2].empty() && !net )
                        throw std::runtime_error( "No net " + field[2] );

                    static const std::map<std::string, ZONE_CONNECTION> connections = {
                        { "solid", ZONE_CONNECTION::FULL }, { "thermal", ZONE_CONNECTION::THERMAL },
                        { "none", ZONE_CONNECTION::NONE }, { "tht_thermal", ZONE_CONNECTION::THT_THERMAL } };
                    const double priority = number( 3 ), clearance = number( 4 ), minimum = number( 5 );
                    const double gap = number( 7 ), spoke = number( 8 );

                    if( !connections.count( field[6] ) || ( field[9] != "solid" && field[9] != "hatched" ) )
                        throw std::runtime_error( "Pads connect solid, thermal, tht_thermal or none; a fill is solid or hatched" );

                    if( priority < 0 || priority > 1000 || priority != std::floor( priority ) || clearance < 0 || clearance > 25
                        || minimum < 0.01 || minimum > 25 || gap < 0 || gap > 25 || spoke < 0.01 || spoke > 25 )
                        throw std::runtime_error( "Zone priority 0-1000; clearance, gap 0-25 mm; widths 0.01-25 mm" );

                    zone->SetNetCode( net ? net->GetNetCode() : NETINFO_LIST::UNCONNECTED );
                    zone->SetAssignedPriority( static_cast<unsigned>( priority ) );
                    zone->SetLocalClearance( toIU( clearance, 0 ).x );
                    zone->SetMinThickness( toIU( minimum, 0 ).x );
                    zone->SetPadConnection( connections.at( field[6] ) );
                    zone->SetThermalReliefGap( toIU( gap, 0 ).x );
                    zone->SetThermalReliefSpokeWidth( toIU( spoke, 0 ).x );
                    zone->SetFillMode( field[9] == "hatched" ? ZONE_FILL_MODE::HATCH_PATTERN : ZONE_FILL_MODE::POLYGONS );
                    zone->UnFill();
                }
                else if( ( field.size() == 5 && field[0] == "place" ) || ( field.size() == 3 && field[0] == "swap" ) )
                {
                    const bool place = field[0] == "place";
                    LIB_ID     id;

                    if( id.Parse( wxString::FromUTF8( field[place ? 1 : 2] ) ) != -1 || !id.IsValid() )
                        throw std::runtime_error( "Expected a footprint as Library:Name" );

                    FOOTPRINT* existing = board.FindFootprintByReference( wxString::FromUTF8( field[place ? 2 : 1] ) );

                    if( place ? existing != nullptr : existing == nullptr )
                        throw std::runtime_error( place ? "The board already has " + field[2] : "No footprint " + field[1] );

                    if( place && field[2].empty() )
                        throw std::runtime_error( "A placed footprint needs a reference" );

                    FOOTPRINT* footprint = loadFootprint( board, id );

                    if( !footprint )
                        throw std::runtime_error( field[place ? 1 : 2] + " is not in the footprint libraries" );

                    footprint->SetFPID( id );
                    footprint->FixUpPadsForBoard( &board );

                    if( place )
                    {
                        // PCB_BASE_FRAME::loadFootprint + PlaceFootprint: the reference given, at (x, y).
                        footprint->SetReference( wxString::FromUTF8( field[2] ) );
                        footprint->SetParent( &board );
                        footprint->SetPosition( toIU( number( 3 ), number( 4 ) ) );
                        footprint->ClearFlags();
                        board.Add( footprint, ADD_MODE::APPEND );
                    }
                    else
                    {
                        // KiCad's Change Footprint: reference, nets (by pad), position, side and
                        // orientation kept (headless/pcbnew/exchange_footprint.cpp).
                        BOARD_COMMIT commit( static_cast<TOOL_MANAGER*>( nullptr ) );
                        CorvCadExchangeFootprint( &board, existing, footprint, commit );
                        commit.Push( wxEmptyString );
                    }
                }
                else if( field.size() == 7 && field[0] == "dimension" )
                {
                    PCB_LAYER_ID   layer = board.GetLayerID( wxString::FromUTF8( field[1] ) );
                    const VECTOR2I start = toIU( number( 2 ), number( 3 ) ), end = toIU( number( 4 ), number( 5 ) );

                    if( layer == UNDEFINED_LAYER || IsCopperLayer( layer ) || layer == Edge_Cuts || !board.IsLayerEnabled( layer ) )
                        throw std::runtime_error( "Put a dimension on a silkscreen, fab or user layer" );

                    if( start == end )
                        throw std::runtime_error( "A dimension needs two different points" );

                    auto* dimension = new PCB_DIM_ALIGNED( &board );
                    dimension->SetUnitsMode( bds.m_DimensionUnitsMode );
                    dimension->SetUnitsFormat( bds.m_DimensionUnitsFormat );
                    dimension->SetPrecision( bds.m_DimensionPrecision );
                    dimension->SetSuppressZeroes( bds.m_DimensionSuppressZeroes );
                    dimension->SetTextPositionMode( bds.m_DimensionTextPosition );
                    dimension->SetKeepTextAligned( bds.m_DimensionKeepTextAligned );
                    dimension->SetLayer( layer );
                    dimension->SetMirrored( board.IsBackLayer( layer ) );
                    dimension->SetTextSize( bds.GetTextSize( layer ) );
                    dimension->SetTextThickness( bds.GetTextThickness( layer ) );
                    dimension->SetItalic( bds.GetTextItalic( layer ) );
                    dimension->SetLineThickness( bds.GetLineThickness( layer ) );
                    dimension->SetArrowLength( bds.m_DimensionArrowLength );
                    dimension->SetExtensionOffset( bds.m_DimensionExtensionOffset );
                    dimension->SetStart( start );
                    dimension->SetEnd( end );
                    dimension->SetHeight( toIU( number( 6 ), 0 ).x );
                    dimension->Update();
                    board.Add( dimension, ADD_MODE::APPEND );
                }
                else
                {
                    throw std::runtime_error( "Unknown board edit" );
                }

                // The next call loads the written file, as kicad-cli would: KiCad orders tied
                // ratsnest edges by item order, so an edited board in memory can name other
                // unconnected pairs than the same board loaded fresh.
                std::string text = saveText( board );
                closeBoard();
                return text;
            },
            aError, aCapacity );
}

// KiCad's board import as PCB_EDIT_FRAME::OpenProjectFiles runs it for a non-KiCad file with
// KICTL_IMPORT_LIB, without the frame and its interactive repair and rehatch actions (which
// kicad-cli pcb import leaves out too): the format's PCB_IO plugin loads `input` with the project
// that owns `output` (its .kicad_pro must exist), nets are listed, the importer's footprints are
// reconciled into the project's footprint cache library and the board re-linked to it
// (FOOTPRINT_IMPORT_RECONCILER), and KiCad's writer saves it at `output`. Formats as kicad-cli
// pcb import's --format, plus circuitmaker, circuitstudio, easyeda and easyedapro.
// Returns "<format>\t<footprints>\t<tracks>\t<vias>\t<zones>\t<importer messages>".
char* cc_board_native_import( const char* aInput, const char* aFormat, const char* aOutput, char* aError,
                              size_t aCapacity )
{
    return invoke(
            [&]
            {
                if( !aInput || !aOutput )
                    throw std::runtime_error( "Missing import paths" );

                start();
                const wxString     input = wxString::FromUTF8( aInput ), output = wxString::FromUTF8( aOutput );
                const std::string  format = aFormat ? aFormat : "auto";
                static const std::map<std::string, PCB_IO_MGR::PCB_FILE_T> formats = {
                    { "pads", PCB_IO_MGR::PADS }, { "altium", PCB_IO_MGR::ALTIUM_DESIGNER },
                    { "circuitmaker", PCB_IO_MGR::ALTIUM_CIRCUIT_MAKER },
                    { "circuitstudio", PCB_IO_MGR::ALTIUM_CIRCUIT_STUDIO }, { "eagle", PCB_IO_MGR::EAGLE },
                    { "cadstar", PCB_IO_MGR::CADSTAR_PCB_ARCHIVE }, { "fabmaster", PCB_IO_MGR::FABMASTER },
                    { "pcad", PCB_IO_MGR::PCAD }, { "solidworks", PCB_IO_MGR::SOLIDWORKS_PCB },
                    { "easyeda", PCB_IO_MGR::EASYEDA }, { "easyedapro", PCB_IO_MGR::EASYEDAPRO } };
                PCB_IO_MGR::PCB_FILE_T type = format == "auto"
                        ? PCB_IO_MGR::FindPluginTypeFromBoardPath( input, 1 << 0 /* KICTL_NONKICAD_ONLY */ )
                        : formats.count( format ) ? formats.at( format ) : PCB_IO_MGR::PCB_FILE_UNKNOWN;
                bool importable = false;

                for( const auto& [name, known] : formats )
                    importable |= known == type;

                if( !importable )
                    throw std::runtime_error( "Not a board format KiCad imports" );

                wxFileName projectFile( output );
                projectFile.SetExt( FILEEXT::ProjectFileExtension );

                if( !projectFile.FileExists() )
                    throw std::runtime_error( "Create the project first: " + utf8( projectFile.GetFullPath() ) );

                CorvCadActivateProject( output );
                SETTINGS_MANAGER& manager = Pgm().GetSettingsManager();
                manager.LoadProject( projectFile.GetFullPath(), true );
                PROJECT&       project = manager.Prj();
                const wxString cache = IMPORT_PROJ_PROPS::MakeCacheNickname( project.GetProjectName() );
                std::map<std::string, UTF8> properties;
                properties[IMPORT_PROJ_PROPS::FP_CACHE_NICKNAME] = UTF8( cache );

                IO_RELEASER<PCB_IO> io( PCB_IO_MGR::FindPlugin( type ) );

                if( !io )
                    throw std::runtime_error( "No KiCad importer for this format" );

                WX_STRING_REPORTER reporter;
                io->SetReporter( &reporter );
                // Projects with several boards or schematics (EasyEDA Pro, Altium projects): the
                // editor asks; headless, the first board-and-schematic pair, else the first one.
                // ponytail: no choice yet; add a design id to the call if users need another one.
                if( auto* chooser = dynamic_cast<PROJECT_CHOOSER_PLUGIN*>( io.get() ) )
                    chooser->RegisterCallback(
                            [&reporter]( const std::vector<IMPORT_PROJECT_DESC>& aDesigns )
                            {
                                std::vector<IMPORT_PROJECT_DESC> chosen;

                                for( const IMPORT_PROJECT_DESC& design : aDesigns )
                                    if( chosen.empty() && !design.PCBId.empty() && !design.SchematicId.empty() )
                                        chosen.push_back( design );

                                if( chosen.empty() && !aDesigns.empty() )
                                    chosen.push_back( aDesigns.front() );

                                if( !chosen.empty() )
                                    reporter.Report( "Imported the first of " + std::to_string( aDesigns.size() ) + " designs: "
                                                     + chosen[0].ComboName + chosen[0].PCBName + chosen[0].SchematicName,
                                                     RPT_SEVERITY_INFO );

                                return chosen;
                            } );
                std::unique_ptr<BOARD> board( io->LoadBoard( input, nullptr, &properties, &project ) );

                if( !board )
                    throw std::runtime_error( "KiCad could not import " + utf8( input ) );

                std::vector<std::unique_ptr<FOOTPRINT>> definitions;

                try
                {
                    for( FOOTPRINT* footprint : io->GetImportedCachedLibraryFootprints() )
                        definitions.emplace_back( footprint );
                }
                catch( const IO_ERROR& )
                {
                    // no cached library: reconcile from the placed footprints only
                }

                board->BuildListOfNets();

                if( FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( &project ) )
                {
                    try
                    {
                        FOOTPRINT_IMPORT_RECONCILER( *adapter, project.GetProjectPath(), &reporter )
                                .Reconcile( board.get(), std::move( definitions ), cache, {} );
                    }
                    catch( const IO_ERROR& error )
                    {
                        reporter.Report( error.What(), RPT_SEVERITY_ERROR ); // as the editor: not fatal
                    }
                }

                // Some importers leave items on no layer (KiCad 10.0.6's Eagle import does for footprint
                // arcs; kicad-cli pcb import saves them so, and KiCad then refuses to open the board
                // headless). KiCad's own answer on opening such a board, Rescue, moves them to
                // User.Comments; do that before saving.
                int rescued = 0;
                const auto rescue = [&]( BOARD_ITEM* aItem )
                {
                    if( aItem->GetLayer() < 0 ) // LSET::Name writes any negative layer as "UNDEFINED"
                    {
                        aItem->SetLayer( Cmts_User );
                        ++rescued;
                    }
                };

                for( BOARD_ITEM* drawing : board->Drawings() )
                    rescue( drawing );

                for( FOOTPRINT* footprint : board->Footprints() )
                {
                    for( BOARD_ITEM* drawing : footprint->GraphicalItems() )
                        rescue( drawing );

                    for( PCB_FIELD* field : footprint->GetFields() )
                        rescue( field );
                }

                if( rescued )
                {
                    board->SetEnabledLayers( LSET( board->GetEnabledLayers() ).set( Cmts_User ) );
                    reporter.Report( wxString::Format( "%d items on undefined layers moved to User.Comments, as KiCad's "
                                                       "Rescue does", rescued ),
                                     RPT_SEVERITY_WARNING );
                }
                board->SetFileName( output );
                IO_RELEASER<PCB_IO> writer( PCB_IO_MGR::FindPlugin( PCB_IO_MGR::KICAD_SEXP ) );
                writer->SaveBoard( output, board.get() );

                size_t vias = 0;

                for( PCB_TRACK* track : board->Tracks() )
                    vias += track->Type() == PCB_VIA_T;

                std::ostringstream report;
                report << format << '\t' << board->Footprints().size() << '\t' << board->Tracks().size() - vias << '\t'
                       << vias << '\t' << board->Zones().size() << '\t'
                       << ( reporter.HasMessage() ? wxSplit( reporter.GetMessages(), '\n' ).size() : 0 );
                return report.str();
            },
            aError, aCapacity );
}

} // extern "C"

namespace
{
// TOOL_BASE::Reset: a new router synced with the open board, in a routing mode.
BOARD& newRouter( const char* aPath, int aMode )
{
    BOARD& board = openBoard( aPath );
    endRoute();

    g_route.iface = std::make_unique<HEADLESS_ROUTER_IFACE>();
    g_route.iface->SetBoard( &board );
    g_route.router = std::make_unique<PNS::ROUTER>();
    g_route.router->SetInterface( g_route.iface.get() );
    g_route.router->ClearWorld();
    g_route.router->SyncWorld();
    g_route.router->UpdateSizes( PNS::SIZES_SETTINGS() );
    routingSettings().SetMode( aMode == 0 ? PNS::RM_MarkObstacles : aMode == 1 ? PNS::RM_Shove : PNS::RM_Walkaround );
    g_route.router->LoadSettings( &routingSettings() );
    return board;
}
} // namespace

extern "C" {

// KiCad's Route Single Track, Route Differential Pair and length tuning on the open board
// (ROUTER_TOOL::prepareInteractive): the start item and layer and its netclass sizes; tuning
// starts on a track and takes the target length or skew.
int cc_board_native_route_start( const char* aPath, double aX, double aY, const char* aLayer, int aMode,
                                 int aKind, double aTarget, char* aError, size_t aCapacity )
{
    char* ok = invoke(
            [&]
            {
                if( aKind < 0 || aKind > 4 )
                    throw std::runtime_error( "Unknown route kind" );

                if( ( aKind == 2 || aKind == 3 ) && aTarget <= 0 )
                    throw std::runtime_error( "Length tuning needs a target length" );

                BOARD& board = newRouter( aPath, aMode );
                static const PNS::ROUTER_MODE modes[] = { PNS::PNS_MODE_ROUTE_SINGLE, PNS::PNS_MODE_ROUTE_DIFF_PAIR,
                                                          PNS::PNS_MODE_TUNE_SINGLE, PNS::PNS_MODE_TUNE_DIFF_PAIR,
                                                          PNS::PNS_MODE_TUNE_DIFF_PAIR_SKEW };
                g_route.router->SetMode( modes[aKind] );

                PCB_LAYER_ID active = board.GetLayerID( wxString::FromUTF8( aLayer ? aLayer : "F.Cu" ) );

                if( !IsCopperLayer( active ) )
                    active = F_Cu;

                const VECTOR2I p = toIU( aX, aY );
                const int      activeLayer = g_route.iface->GetPNSLayerFromBoardLayer( active );
                PNS::ITEM*     start = pickItem( p, nullptr, activeLayer, nullptr, aKind >= 2 ? PICK::TRACKS : PICK::ANY );

                // ROUTER_TOOL::getStartLayer: the active layer if the start item is on it.
                int layer = activeLayer;

                if( start && !start->Layers().Overlaps( activeLayer ) )
                    layer = start->Layers().Start();

                PCB_LAYER_ID        pcbLayer = g_route.iface->GetBoardLayerFromPNSLayer( layer );
                PNS::SIZES_SETTINGS sizes( g_route.router->Sizes() );
                g_route.iface->SetStartLayerFromPCBNew( pcbLayer );
                board.GetDesignSettings().m_TempOverrideTrackWidth = false;
                g_route.iface->ImportSizes( sizes, start, nullptr, VECTOR2D( p ) );
                sizes.AddLayerPair( g_route.iface->GetPNSLayerFromBoardLayer( F_Cu ),
                                    g_route.iface->GetPNSLayerFromBoardLayer( B_Cu ) );
                g_route.router->UpdateSizes( sizes );

                // Tuning starts where the finger is on the track, not at its end.
                if( !g_route.router->StartRouting( aKind >= 2 ? p : snapToItem( start, p ), start, layer ) )
                {
                    std::string reason = utf8( g_route.router->FailureReason() );
                    endRoute();
                    throw std::runtime_error( reason.empty() ? "Cannot start a route here" : reason );
                }

                if( aKind >= 2 )
                {
                    auto* tuner = dynamic_cast<PNS::MEANDER_PLACER_BASE*>( g_route.router->Placer() );
                    PNS::MEANDER_SETTINGS settings = tuner->MeanderSettings();

                    if( aKind == 4 )
                        settings.SetTargetSkew( pcbIUScale.mmToIU( aTarget ) );
                    else
                        settings.SetTargetLength( pcbIUScale.mmToIU( aTarget ) );

                    // The length and delay calculation reads the track's tuning profile from its
                    // netclass (PCB_TUNING_PATTERN sets it the same way).
                    settings.m_netClass = static_cast<BOARD_CONNECTED_ITEM*>( start->Parent() )->GetEffectiveNetClass();

                    tuner->UpdateSettings( settings );
                }

                g_route.startItem = start;
                g_route.kind = aKind;
                return std::string( "1" );
            },
            aError, aCapacity, false );
    const int started = ok ? 1 : 0;
    std::free( ok );
    return started;
}

// ROUTER_TOOL::InlineDrag: drags the track or via under (x, y), its neighbours following as the
// mode allows; route_move moves it, route_fix drops it there, route_finish saves.
int cc_board_native_drag_start( const char* aPath, double aX, double aY, int aMode, char* aError,
                                size_t aCapacity )
{
    char* ok = invoke(
            [&]
            {
                newRouter( aPath, aMode );
                const VECTOR2I p = toIU( aX, aY );
                PNS::ITEM*     item = pickItem( p, nullptr, g_route.iface->GetPNSLayerFromBoardLayer( F_Cu ), nullptr,
                                                PICK::TRACKS_AND_VIAS );

                if( !item )
                {
                    endRoute();
                    throw std::runtime_error( "No track or via to drag here" );
                }

                if( !g_route.router->StartDragging( p, item, PNS::DM_ANY ) )
                {
                    std::string reason = utf8( g_route.router->FailureReason() );
                    endRoute();
                    throw std::runtime_error( reason.empty() ? "Cannot drag this item" : reason );
                }

                g_route.dragging = true;
                return std::string( "1" );
            },
            aError, aCapacity, false );
    const int started = ok ? 1 : 0;
    std::free( ok );
    return started;
}

// ROUTER_TOOL's motion event: the end item under the finger and the router's preview.
char* cc_board_native_route_move( double aX, double aY, char* aError, size_t aCapacity )
{
    return invoke(
            [&]
            {
                PNS::ROUTER& r = router();
                const VECTOR2I p = toIU( aX, aY );
                PNS::ITEM*   end = g_route.dragging ? nullptr : endItem( p );
                const bool   moved = r.Move( g_route.dragging ? p : snapToItem( end, p ), end );

                std::ostringstream out;
                out.imbue( std::locale::classic() );
                const VECTOR2I tip = r.Placer() ? r.Placer()->CurrentEnd() : p;
                out << "end " << pcbIUScale.IUTomm( tip.x ) << ' ' << pcbIUScale.IUTomm( tip.y ) << ' '
                    << utf8( LSET::Name( g_route.iface->GetBoardLayerFromPNSLayer( r.GetCurrentLayer() ) ) ) << '\n';

                if( !moved && !r.FailureReason().IsEmpty() )
                    out << "fail " << utf8( r.FailureReason() ) << '\n';

                // Length tuning: the tuned length (or skew) and whether it meets the target.
                if( auto* tuner = dynamic_cast<PNS::MEANDER_PLACER_BASE*>( r.Placer() ) )
                {
                    static const char* status[] = { "short", "long", "tuned" };
                    out << "tuning " << pcbIUScale.IUTomm( tuner->TuningLengthResult() ) << ' '
                        << status[std::clamp<int>( tuner->TuningStatus(), 0, 2 )] << '\n';
                }

                return out.str() + g_route.iface->m_preview.str();
            },
            aError, aCapacity, false );
}

// ROUTER_TOOL's click: fixes the route up to (x, y); with a via toggled, it is placed there and
// the route continues on the pair's other layer (switchLayerOnViaPlacement).
int cc_board_native_route_fix( double aX, double aY, char* aError, size_t aCapacity )
{
    int   result = -1;
    char* ok = invoke(
            [&]
            {
                PNS::ROUTER&   r = router();
                const VECTOR2I p = toIU( aX, aY );

                // A drag drops where the finger is (DRAGGER::FixRoute commits); a tuning fix
                // commits the meanders (MEANDER_PLACER::FixRoute).
                if( g_route.dragging || dynamic_cast<PNS::MEANDER_PLACER_BASE*>( r.Placer() ) )
                {
                    r.Move( p, nullptr );

                    // The meander placers add the tuned line but leave the original in their
                    // node: PCB_TUNING_PATTERN removes the original tracks itself. Remove the
                    // tuned line (both lines of a pair) so the commit replaces it.
                    PNS::NODE* node = r.Placer() ? r.Placer()->CurrentNode( false ) : nullptr;
                    auto*      start = dynamic_cast<PNS::LINKED_ITEM*>( g_route.startItem );

                    if( node && start && !g_route.dragging )
                    {
                        PNS::DIFF_PAIR pair;

                        if( g_route.kind == 3 && PNS::TOPOLOGY( node ).AssembleDiffPair( start, pair ) )
                        {
                            node->Remove( pair.PLine() );
                            node->Remove( pair.NLine() );
                        }
                        else
                        {
                            PNS::LINE origin = node->AssembleLine( start );
                            node->Remove( origin );
                        }
                    }

                    if( !r.FixRoute( p, nullptr, false, true ) )
                        throw std::runtime_error( "Cannot drop it here: it collides" );

                    g_route.fixed = true;
                    result = 1;
                    return std::string();
                }

                PNS::ITEM*     end = endItem( p );
                const VECTOR2I before = r.Placer()->CurrentStart();
                const bool     placingVia = r.IsPlacingVia();
                const int      layer = r.GetCurrentLayer();

                // A click is preceded by the motion to it (a tap may come without a drag).
                const VECTOR2I target = snapToItem( end, p );
                r.Move( target, end );

                // FixRoute ends the route on any item of the net it is given, reached or not; a
                // tap can come before the route reaches the pad (the editor's user clicks once
                // it has). Short of it, fix what was routed and continue from there instead.
                if( end && r.Placer()->CurrentEnd() != target )
                    end = nullptr;

                if( r.FixRoute( target, end, false, false ) )
                {
                    result = 1;
                    return std::string();
                }

                if( r.Placer()->CurrentStart() == before )
                    throw std::runtime_error( "The route cannot end here: it is empty or collides" );

                if( placingVia )
                {
                    const PNS::SIZES_SETTINGS& sizes = r.Sizes();
                    r.SwitchLayer( layer == sizes.GetLayerTop() ? sizes.GetLayerBottom() : sizes.GetLayerTop() );
                }

                result = 0;
                return std::string();
            },
            aError, aCapacity, false );
    if( !ok )
        return -1;

    std::free( ok );
    return result;
}

int cc_board_native_route_toggle_via( void )
{
    if( !g_route.router || !g_route.router->RoutingInProgress() )
        return 0;

    g_route.router->ToggleViaPlacement();
    return g_route.router->IsPlacingVia() ? 1 : 0;
}

// ROUTER_TOOL's end of routing: CommitRouting pushes what was fixed through the headless
// BOARD_COMMIT.
char* cc_board_native_route_finish( char* aError, size_t aCapacity )
{
    return invoke(
            [&]
            {
                PNS::ROUTER& r = router();
                const bool   placed = g_route.fixed || ( r.Placer() && r.Placer()->HasPlacedAnything() );
                r.CommitRouting();
                endRoute();

                if( !placed )
                    throw std::runtime_error( "Nothing was routed" );

                return saveText( *g_open.board );
            },
            aError, aCapacity );
}

void cc_board_native_route_cancel( void ) { endRoute(); }

} // extern "C"
