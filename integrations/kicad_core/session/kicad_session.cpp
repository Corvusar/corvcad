// SPDX-License-Identifier: GPL-3.0-or-later
// CorvCAD native schematic session over KiCad 10.0.6 (port gate 3).
//
// Implements engine/include/corvcad/schematic_session_c_api.h on KiCad's own SCHEMATIC model:
// loading through kicad-cli's loader, edits on KiCad items, saving through KiCad's SCH_IO
// writer. Snapshots use the compatible backend's JSON schema and ids so the two can be
// diffed. Undo keeps a per-transaction journal: a modified item swaps data with its clone
// (SCH_ITEM::SwapItemData, as SCH_COMMIT::Revert does), an added or removed item moves
// between its screen and the journal. Applying a change twice restores it, so undo and
// redo are the same operation in opposite order.
//
// This file adapts GPL-3.0-or-later KiCad code; a binary linking KiCad sources is a GPL
// combined work. Only the sideloaded NATIVE_KICAD edition of the app links it.

#include "../probe/headless.hpp"

#include <corvcad/schematic_route.hpp>
#include <corvcad/schematic_session_c_api.h>
#include <corvcad/starter_parts.hpp>

#include <drawing_sheet/ds_data_model.h>
#include <import_proj_properties.h>
#include <io/common/plugin_common_choose_project.h>
#include <sch_footprint_field_reconciler.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <eeschema_helpers.h>
#include <eeschema_settings.h>
#include <erc/erc.h>
#include <erc/erc_report.h>
#include <erc/erc_settings.h>
#include <io/io_mgr.h>
#include <lib_symbol.h>
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <paths.h>
#include <wildcards_and_files_ext.h>

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/utils.h>
#include <libraries/symbol_library_adapter.h>
#include <sch_pin.h>
#include <netlist_exporters/netlist_exporter_kicad.h>
#include <project_sch.h>
#include <richio.h>
#include <sch_io/sch_io.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <sch_io/sch_io_mgr.h>
#include <sch_bus_entry.h>
#include <sch_junction.h>
#include <sch_label.h>
#include <sch_plotter.h>
#include <sch_reference_list.h>
#include <sch_render_settings.h>
#include <settings/settings_manager.h>
#include <font/font.h>
#include <font/kicad_font_name.h>
#include <reporter.h>
#include <sch_sheet_pin.h>
#include <common.h>
#include <string_utils.h>
#include <sch_line.h>
#include <sch_no_connect.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <symbol_editor/symbol_editor_settings.h>
#ifdef CORVCAD_NGSPICE_STATIC
#include <project/project_file.h>
#include <schematic_settings.h>
#include <sim/spice_circuit_model.h>
#include <sim/spice_settings.h>
#include <sim/spice_simulator.h>
#include <algorithm>
#include <atomic>
#include <thread>
#endif

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace
{
// Open schematic sessions. Native model calls are serialized by the host.
int g_liveSessions = 0;
}

// The board code's cached board (kicad_board.cpp); none without the board code.
__attribute__( ( weak ) ) void CorvCadCloseBoard() {}

// Internal shared policy for the schematic and board adapters, called before a model loads.
// KiCad resolves every library (project tables, ${KIPRJMOD}) through its one active project,
// so a project must be active for its own libraries; but activating another project destroys
// the previous PROJECT under any model still using it (a standalone schematic can borrow the
// active one too). So the project becomes active only while no schematic session is open: the
// cached board is closed and every loaded project unloaded, and the next load becomes the
// active one. Otherwise it loads beside the active project and sees that one's libraries.
// Returns the aSetActive argument for LoadSchematic and LoadBoard.
bool CorvCadActivateProject( const wxString& aModelFile )
{
    SETTINGS_MANAGER& manager = Pgm().GetSettingsManager();
    wxFileName        projectFile( aModelFile );
    projectFile.SetExt( FILEEXT::ProjectFileExtension );

    if( manager.Prj().GetProjectFullName() == projectFile.GetFullPath() || g_liveSessions > 0 )
        return false;

    CorvCadCloseBoard();

    for( const wxString& path : manager.GetOpenProjects() )
        manager.UnloadProject( manager.GetProject( path ), false );

    return true;
}

// The one module object of a NATIVE_KICAD build; the board code (kicad_board.cpp) uses it too.
// Neither module installs its settings here, so the board code's Kiface().KifaceSettings()
// uses see none, as in the headless board probe.
KIFACE_BASE& Kiface()
{
    static HEADLESS_KIFACE kiface( "eeschema", KIWAY::FACE_SCH );
    return kiface;
}

// The CvPcb module ERC's footprint link check asks. kicad_board.cpp defines the real one in
// NATIVE_KICAD builds; without the board code there is none and the check is skipped.
__attribute__( ( weak ) ) KIFACE* CorvCadFootprintLinkKiface()
{
    return nullptr;
}

// Starts the window-less KiCad program once; kicad_board.cpp adds the board settings after it.
bool CorvCadStartKiCad()
{
    static const bool started = []
    {
        static char  name[] = "corvcad";
        static char* argv[] = { name, nullptr };

        if( !StartHeadless( 1, argv ) )
            return false;

        SETTINGS_MANAGER& manager = Pgm().GetSettingsManager();
        manager.RegisterSettings( new EESCHEMA_SETTINGS, false );
        manager.RegisterSettings( new SYMBOL_EDITOR_SETTINGS, false );
        manager.Load();

        // The official symbol and footprint libraries bundled with the app (KICAD10_SYMBOL_DIR
        // and KICAD10_FOOTPRINT_DIR, set by the host): install their global tables where KiCad
        // keeps them, as a first run of KiCad would, and load them as kicad-cli does at start.
        // Project tables still come first when a project resolves its libraries.
        const struct { const wchar_t* variable; std::string table; LIBRARY_TABLE_TYPE type; } bundles[] = {
            { L"KICAD10_SYMBOL_DIR", FILEEXT::SymbolLibraryTableFileName, LIBRARY_TABLE_TYPE::SYMBOL },
            { L"KICAD10_FOOTPRINT_DIR", FILEEXT::FootprintLibraryTableFileName, LIBRARY_TABLE_TYPE::FOOTPRINT },
        };

        for( const auto& bundle : bundles )
        {
            wxString folder;

            if( !wxGetEnv( bundle.variable, &folder ) )
                continue;

            wxFileName bundled( folder, bundle.table );
            wxFileName global( PATHS::GetUserSettingsPath(), bundle.table );

            if( bundled.FileExists() && wxFileName::Mkdir( global.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL )
                && wxCopyFile( bundled.GetFullPath(), global.GetFullPath(), true ) )
            {
                Pgm().GetLibraryManager().LoadGlobalTables( { bundle.type } );
            }
        }

        return true;
    }();

    return started;
}

namespace
{
const char kBackend[] = "kicad-10.0.6-native";

struct ERROR : std::runtime_error
{
    ERROR( int aCode, const std::string& aMessage ) : std::runtime_error( aMessage ), code( aCode ) {}
    int code;
};

// One journalled change. `held` is the clone of a modified item, or an added or removed
// item while it is off its screen.
struct CHANGE
{
    enum KIND { MODIFY, ADD, REMOVE } kind;
    SCH_SCREEN*               screen;
    SCH_ITEM*                 item;
    std::unique_ptr<SCH_ITEM> held;
};

void flip( CHANGE& aChange )
{
    if( aChange.kind == CHANGE::MODIFY )
    {
        aChange.item->SwapItemData( aChange.held.get() );
        aChange.screen->Update( aChange.item );

        if( aChange.item->Type() == SCH_SYMBOL_T )
            static_cast<SCH_SYMBOL*>( aChange.item )->UpdatePins();
    }
    else if( aChange.held )
    {
        aChange.screen->Append( aChange.held.release() );
    }
    else
    {
        aChange.screen->Remove( aChange.item );
        aChange.held.reset( aChange.item );
    }
}

using JOURNAL = std::vector<CHANGE>;
} // namespace

// Member order matters: the journals own off-screen items and must go before the schematic.
struct cc_sch_session
{
    std::unique_ptr<SCHEMATIC>         schematic;
    fs::path                           folder;
    std::string                        project;
    std::vector<JOURNAL>               history;
    size_t                             index = 0; // history[0, index) is applied
    std::set<SCH_SCREEN*>              dirty;
    std::map<SCH_SCREEN*, fs::file_time_type> loaded;
    bool                               open = false;
};

struct cc_sch_transaction
{
    cc_sch_session* session;
    JOURNAL         changes;
    bool            active = true;
};

namespace
{
char* owned( const std::string& aValue )
{
    char* text = static_cast<char*>( std::malloc( aValue.size() + 1 ) );

    if( !text )
        throw std::bad_alloc();

    std::memcpy( text, aValue.c_str(), aValue.size() + 1 );
    return text;
}

void setResult( cc_sch_result* aResult, int aCode, const std::string& aMessage )
{
    if( !aResult )
        return;

    std::free( aResult->message );
    aResult->code = aCode;
    aResult->message = aCode ? owned( aMessage ) : nullptr;
}

template <class F>
auto invoke( F&& aOperation, cc_sch_result* aResult, decltype( aOperation() ) aFailed )
{
    try
    {
        auto value = aOperation();
        setResult( aResult, CC_SCH_OK, {} );
        return value;
    }
    catch( const ERROR& error )
    {
        setResult( aResult, error.code, error.what() );
    }
    catch( const IO_ERROR& error )
    {
        setResult( aResult, CC_SCH_IO, error.What().ToStdString() );
    }
    catch( const std::exception& error )
    {
        setResult( aResult, CC_SCH_INVALID, error.what() );
    }

    return aFailed;
}

std::string required( const char* aText, const char* aName )
{
    if( !aText )
        throw ERROR( CC_SCH_INVALID, std::string( "Missing " ) + aName );

    return aText;
}

int iu( double aMillimetres ) { return schIUScale.mmToIU( aMillimetres ); }
double mm( int aIU ) { return schIUScale.IUTomm( aIU ); }
std::string utf8( const wxString& aText ) { return aText.utf8_string(); }

void startKiCad()
{
    if( !CorvCadStartKiCad() )
        throw ERROR( CC_SCH_IO, "KiCad core did not start" );
}

SCH_SHEET_PATH sheetPath( const cc_sch_session& aSession, const char* aPath )
{
    const std::string path = required( aPath, "sheet path" );

    for( const SCH_SHEET_PATH& sheet : aSession.schematic->Hierarchy() )
    {
        if( utf8( sheet.Path().AsString() ) == path )
            return sheet;
    }

    throw ERROR( CC_SCH_NOT_FOUND, "Unknown schematic sheet path" );
}

template <class T = SCH_ITEM>
T* findItem( const SCH_SHEET_PATH& aSheet, const char* aUuid )
{
    const std::string uuid = required( aUuid, "item UUID" );

    for( SCH_ITEM* item : aSheet.LastScreen()->Items() )
    {
        if( item->m_Uuid.AsStdString() == uuid )
        {
            if( T* typed = dynamic_cast<T*>( item ) )
                return typed;

            throw ERROR( CC_SCH_INVALID, "Item has a different type" );
        }
    }

    throw ERROR( CC_SCH_NOT_FOUND, "Schematic item not found" );
}

JOURNAL& journal( cc_sch_transaction* aTransaction )
{
    if( !aTransaction || !aTransaction->active )
        throw ERROR( CC_SCH_INVALID, "Transaction is closed" );

    return aTransaction->changes;
}

const cc_sch_session& owner( cc_sch_transaction* aTransaction )
{
    journal( aTransaction );
    return *aTransaction->session;
}

// Journals the item's current state, then edits it in place.
template <class T, class F>
void modify( cc_sch_transaction* aTransaction, const SCH_SHEET_PATH& aSheet, T* aItem, F&& aEdit )
{
    JOURNAL& changes = journal( aTransaction );
    changes.push_back( { CHANGE::MODIFY, aSheet.LastScreen(), aItem,
                         std::unique_ptr<SCH_ITEM>( static_cast<SCH_ITEM*>( aItem->Clone() ) ) } );
    aEdit( aItem );
    aSheet.LastScreen()->Update( aItem );
}

void add( cc_sch_transaction* aTransaction, const SCH_SHEET_PATH& aSheet, SCH_ITEM* aItem )
{
    std::unique_ptr<SCH_ITEM> item( aItem );
    JOURNAL& changes = journal( aTransaction );
    aSheet.LastScreen()->Append( item.get() );
    changes.push_back( { CHANGE::ADD, aSheet.LastScreen(), item.release(), nullptr } );
}

// A new wire end on another wire's interior, or where three or more directions meet, needs
// the explicit junction dot the editor would add (SCH_SCREEN::IsExplicitJunctionNeeded).
void addJunctions( cc_sch_transaction* aTransaction, const SCH_SHEET_PATH& aSheet,
                   std::initializer_list<VECTOR2I> aPoints )
{
    for( const VECTOR2I& point : aPoints )
    {
        if( aSheet.LastScreen()->IsExplicitJunctionNeeded( point ) )
            add( aTransaction, aSheet, new SCH_JUNCTION( point ) );
    }
}

// The editor's path after a commit, without cleanup: KiCad's cleanup merges wires and adds
// junctions, which would be edits outside the journal.
// ponytail: full recalculation; pass the changed items for KiCad's incremental update if
// large schematics make edits slow.
void connect( cc_sch_session& aSession )
{
    aSession.schematic->RefreshHierarchy(); // a sheet may have been added or removed
    aSession.schematic->RecalculateConnections( nullptr, NO_CLEANUP, nullptr );
}

void finish( cc_sch_session& aSession, JOURNAL& aChanges, bool aForward )
{
    if( aForward )
        for( CHANGE& change : aChanges )
            flip( change );
    else
        for( auto it = aChanges.rbegin(); it != aChanges.rend(); ++it )
            flip( *it );

    for( CHANGE& change : aChanges )
        aSession.dirty.insert( change.screen );

    connect( aSession );
}

// What a touch-drawn wire must not cross (engine/include/corvcad/schematic_route.hpp), from
// KiCad's model: every pin of every symbol, not just CorvCAD's two-pin part, and each
// symbol's body and pins box. Decorative graphics can extend past a pin tip (LED light
// arrows do), so each pin gets a narrow outward escape through its own symbol's box.
corvcad::RouteObstacles obstacles( const SCH_SHEET_PATH& aSheet )
{
    corvcad::RouteObstacles result;
    auto point = [&]( const VECTOR2I& aPoint ) { result.points.push_back( { mm( aPoint.x ), mm( aPoint.y ) } ); };

    for( SCH_ITEM* item : aSheet.LastScreen()->Items() )
    {
        if( SCH_SYMBOL* symbol = dynamic_cast<SCH_SYMBOL*>( item ) )
        {
            // The body plus the pin tips, not GetBodyAndPinsBoundingBox(), which also covers
            // pin name and number text and would swallow the tips a route must leave from.
            BOX2I box = symbol->GetBodyBoundingBox();

            for( SCH_PIN* pin : symbol->GetPins( &aSheet ) )
            {
                point( pin->GetPosition() );
                box.Merge( pin->GetPosition() );
            }

            box.Inflate( -iu( 0.01 ) );
            corvcad::RouteObstacles body;
            body.boxes.push_back( { mm( box.GetLeft() ), mm( box.GetTop() ), mm( box.GetRight() ),
                                    mm( box.GetBottom() ) } );

            for( SCH_PIN* pin : symbol->GetPins( &aSheet ) )
            {
                const VECTOR2I tip = pin->GetPosition();
                const VECTOR2I outward = tip - pin->GetPinRoot();
                corvcad::open_pin_escape( body, { mm( tip.x ), mm( tip.y ) },
                                         { mm( outward.x ), mm( outward.y ) } );
            }

            result.boxes.insert( result.boxes.end(), body.boxes.begin(), body.boxes.end() );
        }
        else if( SCH_LINE* line = dynamic_cast<SCH_LINE*>( item ); line && line->IsWire() )
        {
            point( line->GetStartPoint() );
            point( line->GetEndPoint() );
        }
        else if( item->Type() == SCH_JUNCTION_T || item->Type() == SCH_LABEL_T
                 || item->Type() == SCH_GLOBAL_LABEL_T || item->Type() == SCH_HIER_LABEL_T )
        {
            point( item->GetPosition() );
        }
    }

    return result;
}

// Runs aWrite on a temporary file and returns what it wrote.
template <class F>
std::string throughFile( const char* aExtension, F&& aWrite )
{
    const fs::path file = fs::temp_directory_path() / ( "corvcad-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count() ) + aExtension );
    aWrite( wxString::FromUTF8( file.string() ) );
    std::ifstream      stream( file, std::ios::binary );
    std::ostringstream text;
    text << stream.rdbuf();
    stream.close();
    fs::remove( file );
    return text.str();
}

std::string quote( const std::string& aValue )
{
    std::ostringstream out;
    out << '"';

    for( unsigned char c : aValue )
    {
        switch( c )
        {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if( c < 32 )
                out << "\\u" << std::hex << std::setw( 4 ) << std::setfill( '0' ) << int( c ) << std::dec;
            else
                out << char( c );
        }
    }

    return out.str() + '"';
}

std::string relativeFile( const cc_sch_session& aSession, const SCH_SCREEN* aScreen )
{
    return fs::weakly_canonical( fs::path( utf8( aScreen->GetFileName() ) ) )
            .lexically_relative( aSession.folder ).generic_string();
}

// Same fields and order as the compatible backend (engine/src/schematic_session_c_api.cpp).
std::string snapshot( const cc_sch_session& aSession )
{
    std::ostringstream out;
    out.imbue( std::locale::classic() );
    out << std::setprecision( 17 ) << "{\"backend\":\"" << kBackend
        << "\",\"kiCadErcAvailable\":false,\"sheets\":[";

    SCH_SHEET_LIST           sheets = aSession.schematic->Hierarchy();
    std::map<SCH_SCREEN*, int> instances;

    for( const SCH_SHEET_PATH& sheet : sheets )
        ++instances[sheet.LastScreen()];

    bool firstSheet = true;

    for( const SCH_SHEET_PATH& sheet : sheets )
    {
        SCH_SCREEN*       screen = sheet.LastScreen();
        const std::string path = utf8( sheet.Path().AsString() );
        const std::string name = sheet.Path().size() == 1 ? aSession.project : utf8( sheet.Last()->GetName() );
        std::ostringstream symbols, wires, buses, labels, junctions, noConnects;

        for( std::ostringstream* list : { &symbols, &wires, &buses, &labels, &junctions, &noConnects } )
        {
            list->imbue( std::locale::classic() );
            *list << std::setprecision( 17 );
        }

        auto item = [&]( std::ostringstream& aList, const SCH_ITEM* aItem ) -> std::ostringstream&
        {
            if( aList.tellp() > 0 )
                aList << ',';

            aList << "{\"id\":" << quote( path + ":" + aItem->m_Uuid.AsStdString() )
                  << ",\"uuid\":" << quote( aItem->m_Uuid.AsStdString() );
            return aList;
        };

        for( const SCH_ITEM* schItem : screen->Items() )
        {
            if( const SCH_SYMBOL* symbol = dynamic_cast<const SCH_SYMBOL*>( schItem ) )
            {
                // The writer's angle (sch_io_kicad_sexpr.cpp saveSymbol), without mirroring.
                const int orientation = symbol->GetOrientation() & ~( SYM_MIRROR_X | SYM_MIRROR_Y );
                const int rotation = orientation == SYM_ORIENT_90    ? 90
                                     : orientation == SYM_ORIENT_180 ? 180
                                     : orientation == SYM_ORIENT_270 ? 270
                                                                     : 0;
                item( symbols, symbol )
                        << ",\"reference\":" << quote( utf8( symbol->GetRef( &sheet ) ) )
                        << ",\"value\":" << quote( utf8( symbol->GetField( FIELD_T::VALUE )->GetText() ) )
                        << ",\"libraryId\":" << quote( symbol->GetLibId().Format().c_str() )
                        << ",\"footprint\":" << quote( utf8( symbol->GetField( FIELD_T::FOOTPRINT )->GetText() ) )
                        << ",\"x\":" << mm( symbol->GetPosition().x ) << ",\"y\":" << mm( symbol->GetPosition().y )
                        << ",\"rotation\":" << rotation << '}';
            }
            else if( const SCH_LINE* line = dynamic_cast<const SCH_LINE*>( schItem ) )
            {
                if( !line->IsWire() && !line->IsBus() )
                    continue;

                item( line->IsWire() ? wires : buses, line )
                        << ",\"x0\":" << mm( line->GetStartPoint().x ) << ",\"y0\":" << mm( line->GetStartPoint().y )
                        << ",\"x1\":" << mm( line->GetEndPoint().x ) << ",\"y1\":" << mm( line->GetEndPoint().y ) << '}';
            }
            else if( schItem->Type() == SCH_LABEL_T || schItem->Type() == SCH_GLOBAL_LABEL_T
                     || schItem->Type() == SCH_HIER_LABEL_T )
            {
                const auto* label = static_cast<const SCH_LABEL_BASE*>( schItem );
                const char* kind = schItem->Type() == SCH_LABEL_T          ? "label"
                                   : schItem->Type() == SCH_GLOBAL_LABEL_T ? "global_label"
                                                                           : "hierarchical_label";
                item( labels, label ) << ",\"text\":" << quote( utf8( label->GetText() ) )
                                      << ",\"kind\":\"" << kind << "\",\"x\":" << mm( label->GetPosition().x )
                                      << ",\"y\":" << mm( label->GetPosition().y ) << '}';
            }
            else if( schItem->Type() == SCH_JUNCTION_T || schItem->Type() == SCH_NO_CONNECT_T )
            {
                item( schItem->Type() == SCH_JUNCTION_T ? junctions : noConnects, schItem )
                        << ",\"x\":" << mm( schItem->GetPosition().x )
                        << ",\"y\":" << mm( schItem->GetPosition().y ) << '}';
            }
        }

        if( !firstSheet )
            out << ',';

        firstSheet = false;
        out << "{\"path\":" << quote( path ) << ",\"file\":" << quote( relativeFile( aSession, screen ) )
            << ",\"name\":" << quote( name ) << ",\"uuid\":" << quote( screen->GetUuid().AsStdString() )
            << ",\"version\":" << screen->GetFileFormatVersionAtLoad()
            << ",\"sharedFile\":" << ( instances[screen] > 1 ? "true" : "false" )
            << ",\"unsupportedConstructs\":[],\"symbols\":[" << symbols.str() << "],\"wires\":["
            << wires.str() << "],\"buses\":[" << buses.str() << "],\"labels\":[" << labels.str()
            << "],\"junctions\":[" << junctions.str() << "],\"noConnects\":[" << noConnects.str() << "]}";
    }

    return out.str() + "]}";
}

void writeScreen( cc_sch_session& aSession, SCH_SCREEN* aScreen, const fs::path& aFile )
{
    SCH_SHEET* sheet = aSession.schematic->Hierarchy().FindSheetForScreen( aScreen ).Last();
    IO_RELEASER<SCH_IO> io( SCH_IO_MGR::FindPlugin( SCH_IO_MGR::SCH_KICAD ) );
    io->SaveSchematicFile( wxString::FromUTF8( aFile.string() ), sheet, aSession.schematic.get() );
}

void requireIdle( const cc_sch_session* aSession )
{
    if( !aSession )
        throw ERROR( CC_SCH_INVALID, "Missing schematic session" );

    if( aSession->open )
        throw ERROR( CC_SCH_INVALID, "Finish the schematic transaction first" );
}

int done() { return 1; }

// Placement rules shared by add_two_pin and add_library_symbol: not on a shared sheet, at least
// 10.16 mm from any symbol; returns the first reference <prefix><n> no sheet instance uses.
wxString freeReference( const cc_sch_session& aSession, const SCH_SHEET_PATH& aSheet, double aX, double aY,
                        const wxString& aPrefix )
{
    SCH_SCREEN*        screen = aSheet.LastScreen();
    std::set<wxString> taken;
    int                instances = 0;

    for( const SCH_SHEET_PATH& other : aSession.schematic->Hierarchy() )
    {
        instances += other.LastScreen() == screen;

        for( SCH_ITEM* item : other.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
            taken.insert( static_cast<SCH_SYMBOL*>( item )->GetRef( &other ) );
    }

    if( instances > 1 )
        throw ERROR( CC_SCH_UNSUPPORTED, "Adding a part to a shared sheet is not supported yet" );

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
    {
        if( std::hypot( mm( item->GetPosition().x ) - aX, mm( item->GetPosition().y ) - aY ) < 10.16 )
            throw ERROR( CC_SCH_INVALID, "Place the part farther from an existing symbol" );
    }

    int number = 1;

    while( taken.count( aPrefix + wxString::Format( wxS( "%d" ), number ) ) )
        ++number;

    return aPrefix + wxString::Format( wxS( "%d" ), number );
}
} // namespace

extern "C" {

// The engine's c_api.cpp defines the same function; this one serves a standalone link.
__attribute__( ( weak ) ) void cc_free_text( char* aText ) { std::free( aText ); }

int cc_sch_api_version( void ) { return 11; }

void cc_sch_result_clear( cc_sch_result* aResult )
{
    if( !aResult )
        return;

    std::free( aResult->message );
    aResult->message = nullptr;
    aResult->code = 0;
}

cc_sch_session* cc_sch_session_open( const char* aPath, cc_sch_result* aResult )
{
    return invoke(
            [&]() -> cc_sch_session*
            {
                fs::path root = fs::absolute( fs::path( required( aPath, "schematic path" ) ) );

                if( root.extension() == ".kicad_pro" )
                    root.replace_extension( ".kicad_sch" );

                if( root.extension() != ".kicad_sch" || !fs::is_regular_file( root ) )
                    throw ERROR( CC_SCH_INVALID, "Open a KiCad .kicad_pro or .kicad_sch file" );

                startKiCad();
                auto session = std::make_unique<cc_sch_session>();
                const wxString file = wxString::FromUTF8( root.string() );
                session->schematic.reset( EESCHEMA_HELPERS::LoadSchematic(
                        file, SCH_IO_MGR::SCH_KICAD, CorvCadActivateProject( file ), false, nullptr, true ) );

                if( !session->schematic )
                    throw ERROR( CC_SCH_IO, "KiCad could not load " + root.string() );

                session->folder = fs::canonical( root.parent_path() );
                session->project = root.stem().string();

                for( const SCH_SHEET_PATH& sheet : session->schematic->Hierarchy() )
                    session->loaded[sheet.LastScreen()] =
                            fs::last_write_time( fs::path( utf8( sheet.LastScreen()->GetFileName() ) ) );

                ++g_liveSessions;
                return session.release();
            },
            aResult, nullptr );
}

void cc_sch_session_free( cc_sch_session* aSession )
{
    if( !aSession )
        return;

    delete aSession;
    --g_liveSessions;
}

const char* cc_sch_session_backend( const cc_sch_session* aSession )
{
    return aSession ? kBackend : nullptr;
}

unsigned cc_sch_session_capabilities( const cc_sch_session* aSession )
{
    return aSession ? CC_SCH_TRANSACTIONS | CC_SCH_HIERARCHY | CC_SCH_NATIVE_KICAD | CC_SCH_ERC
                              | CC_SCH_NETLIST | CC_SCH_LIBRARY | CC_SCH_KICAD_TOOLS | CC_SCH_PLOT
#ifdef CORVCAD_NGSPICE_STATIC
                              | CC_SCH_SIMULATE
#endif
                    : 0;
}

char* cc_sch_session_snapshot_json( const cc_sch_session* aSession, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                if( !aSession )
                    throw ERROR( CC_SCH_INVALID, "Missing schematic session" );

                return owned( snapshot( *aSession ) );
            },
            aResult, static_cast<char*>( nullptr ) );
}

cc_sch_transaction* cc_sch_transaction_begin( cc_sch_session* aSession, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                requireIdle( aSession );
                aSession->open = true;
                return new cc_sch_transaction{ aSession, {} };
            },
            aResult, static_cast<cc_sch_transaction*>( nullptr ) );
}

int cc_sch_transaction_move_symbol( cc_sch_transaction* aTransaction, const char* aPath,
                                    const char* aUuid, double aX, double aY, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                modify( aTransaction, sheet, findItem<SCH_SYMBOL>( sheet, aUuid ),
                        [&]( SCH_SYMBOL* aSymbol ) { aSymbol->SetPosition( VECTOR2I( iu( aX ), iu( aY ) ) ); } );
                return done();
            },
            aResult, 0 );
}

int cc_sch_transaction_rotate_symbol( cc_sch_transaction* aTransaction, const char* aPath,
                                      const char* aUuid, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                modify( aTransaction, sheet, findItem<SCH_SYMBOL>( sheet, aUuid ),
                        []( SCH_SYMBOL* aSymbol ) { aSymbol->Rotate( aSymbol->GetPosition(), true ); } );
                return done();
            },
            aResult, 0 );
}

int cc_sch_transaction_set_property( cc_sch_transaction* aTransaction, const char* aPath,
                                     const char* aUuid, const char* aProperty, const char* aValue,
                                     cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH  sheet = sheetPath( owner( aTransaction ), aPath );
                SCH_SYMBOL*     symbol = findItem<SCH_SYMBOL>( sheet, aUuid );
                const wxString  name = wxString::FromUTF8( required( aProperty, "property" ) );
                const wxString  value = wxString::FromUTF8( required( aValue, "value" ) );

                if( name == wxS( "Reference" ) )
                    throw ERROR( CC_SCH_INVALID, "Use rename for the reference" );

                if( !symbol->GetField( name ) )
                    throw ERROR( CC_SCH_NOT_FOUND, "Schematic property not found" );

                modify( aTransaction, sheet, symbol,
                        [&]( SCH_SYMBOL* aSymbol ) { aSymbol->GetField( name )->SetText( value ); } );
                return done();
            },
            aResult, 0 );
}

int cc_sch_transaction_rename_symbol( cc_sch_transaction* aTransaction, const char* aPath,
                                      const char* aUuid, const char* aReference, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                const wxString reference = wxString::FromUTF8( required( aReference, "reference" ) );

                if( reference.IsEmpty() )
                    throw ERROR( CC_SCH_INVALID, "Use a reference such as R1" );

                // KiCad keeps one reference per sheet instance, so shared sheets rename here too.
                modify( aTransaction, sheet, findItem<SCH_SYMBOL>( sheet, aUuid ),
                        [&]( SCH_SYMBOL* aSymbol ) { aSymbol->SetRef( &sheet, reference ); } );
                return done();
            },
            aResult, 0 );
}

int cc_sch_transaction_delete_item( cc_sch_transaction* aTransaction, const char* aPath,
                                    const char* aUuid, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                SCH_ITEM*      item = findItem( sheet, aUuid );

                // Version 9: a sheet too, as KiCad's Delete; the hierarchy is refreshed after the
                // commit (connect), and its file stays in the project folder, as in KiCad.
                sheet.LastScreen()->Remove( item );
                journal( aTransaction ).push_back( { CHANGE::REMOVE, sheet.LastScreen(), item,
                                                     std::unique_ptr<SCH_ITEM>( item ) } );
                return done();
            },
            aResult, 0 );
}

int cc_sch_transaction_add_wire( cc_sch_transaction* aTransaction, const char* aPath, double aX0,
                                 double aY0, double aX1, double aY1, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                const VECTOR2I start( iu( aX0 ), iu( aY0 ) ), end( iu( aX1 ), iu( aY1 ) );

                if( start == end )
                    throw ERROR( CC_SCH_INVALID, "A wire needs two different points" );

                auto* wire = new SCH_LINE( start, LAYER_WIRE );
                wire->SetEndPoint( end );
                add( aTransaction, sheet, wire );
                addJunctions( aTransaction, sheet, { start, end } );
                return done();
            },
            aResult, 0 );
}

int cc_sch_transaction_add_label( cc_sch_transaction* aTransaction, const char* aPath,
                                  const char* aName, double aX, double aY, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                const wxString name = wxString::FromUTF8( required( aName, "label" ) );

                if( name.IsEmpty() )
                    throw ERROR( CC_SCH_INVALID, "Enter a net name" );

                add( aTransaction, sheet, new SCH_LABEL( VECTOR2I( iu( aX ), iu( aY ) ), name ) );
                return done();
            },
            aResult, 0 );
}

// CorvCAD's two-pin part from the project's CorvCAD library, with the compatible backend's
// rules: refused on a shared sheet, 10.16 mm from any symbol, and the first R<n> no sheet
// instance uses.
int cc_sch_transaction_add_two_pin( cc_sch_transaction* aTransaction, const char* aPath, double aX,
                                    double aY, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                const cc_sch_session& session = owner( aTransaction );
                SCH_SHEET_PATH        sheet = sheetPath( session, aPath );
                SCH_SCREEN*           screen = sheet.LastScreen();
                const wxString        reference = freeReference( session, sheet, aX, aY, wxS( "R" ) );

                // The sheet's cached CorvCAD:TwoPin, else the same definition the compatible
                // backend embeds, so placement works without a CorvCAD library in the project.
                const LIB_ID libId( wxS( "CorvCAD" ), wxS( "TwoPin" ) );
                std::unique_ptr<LIB_SYMBOL> parsed;
                const LIB_SYMBOL*           library = nullptr;
                const auto                  cached = screen->GetLibSymbols().find( wxS( "CorvCAD:TwoPin" ) );

                if( cached != screen->GetLibSymbols().end() )
                {
                    library = cached->second;
                }
                else
                {
                    std::string text = corvcad::two_pin_symbol_definition;
                    std::vector<LIB_SYMBOL*> symbols = SCH_IO_KICAD_SEXPR::ParseLibSymbols( text, "CorvCAD" );

                    if( symbols.size() != 1 )
                        throw ERROR( CC_SCH_IO, "CorvCAD:TwoPin definition did not parse" );

                    // The parser leaves the library nickname empty; the netlist's libpart needs it.
                    parsed.reset( symbols.front() );
                    parsed->SetLibId( libId );
                    library = parsed.get();
                }

                auto* symbol = new SCH_SYMBOL( *library, libId, &sheet, 1, BODY_STYLE::BASE, VECTOR2I( iu( aX ), iu( aY ) ) );
                symbol->SetRef( &sheet, reference );
                add( aTransaction, sheet, symbol );
                return done();
            },
            aResult, 0 );
}

// API version 5: any symbol of the project's or the bundled libraries ("Library:Name"), unit 1,
// flattened as KiCad's symbol placement does, with add_two_pin's placement rules and the first
// free reference for the symbol's own prefix. Its library loads on demand.
int cc_sch_transaction_add_library_symbol( cc_sch_transaction* aTransaction, const char* aPath,
                                           const char* aLibId, double aX, double aY, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                const cc_sch_session& session = owner( aTransaction );
                SCH_SHEET_PATH        sheet = sheetPath( session, aPath );
                LIB_ID                libId;

                if( libId.Parse( required( aLibId, "library symbol" ) ) >= 0 || !libId.IsValid() )
                    throw ERROR( CC_SCH_INVALID, "Expected a library symbol as Library:Name" );

                SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &session.schematic->Project() );
                const wxString          nickname = libId.GetLibNickname();

                if( !adapter->GetLibraryStatus( nickname ) )
                    adapter->LoadOne( nickname );

                LIB_SYMBOL* library = adapter->LoadSymbol( libId );

                if( !library )
                    throw ERROR( CC_SCH_NOT_FOUND, std::string( "Symbol not found in the libraries: " ) + aLibId );

                std::unique_ptr<LIB_SYMBOL> flat = library->Flatten();
                flat->SetLibId( libId );

                // The library's reference text without its "?" or number: R, U, #PWR.
                wxString prefix = flat->GetReferenceField().GetText();

                while( !prefix.IsEmpty() && ( prefix.Last() == '?' || wxIsdigit( prefix.Last() ) ) )
                    prefix.RemoveLast();

                const wxString reference = freeReference( session, sheet, aX, aY, prefix.IsEmpty() ? wxS( "U" ) : prefix );
                auto* symbol = new SCH_SYMBOL( *flat, libId, &sheet, 1, BODY_STYLE::BASE, VECTOR2I( iu( aX ), iu( aY ) ) );
                symbol->SetRef( &sheet, reference );
                add( aTransaction, sheet, symbol );
                return done();
            },
            aResult, 0 );
}

int cc_sch_transaction_add_routed_wire( cc_sch_transaction* aTransaction, const char* aPath,
                                        double aX0, double aY0, double aX1, double aY1,
                                        cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                const auto     route = corvcad::plan_orthogonal_route( obstacles( sheet ), aX0, aY0, aX1, aY1 );

                for( size_t i = 0; i + 1 < route.size(); ++i )
                {
                    auto* wire = new SCH_LINE( VECTOR2I( iu( route[i].x ), iu( route[i].y ) ), LAYER_WIRE );
                    wire->SetEndPoint( VECTOR2I( iu( route[i + 1].x ), iu( route[i + 1].y ) ) );
                    add( aTransaction, sheet, wire );
                }

                addJunctions( aTransaction, sheet,
                              { VECTOR2I( iu( route.front().x ), iu( route.front().y ) ),
                                VECTOR2I( iu( route.back().x ), iu( route.back().y ) ) } );
                return done();
            },
            aResult, 0 );
}

int cc_sch_transaction_move_label( cc_sch_transaction* aTransaction, const char* aPath,
                                   const char* aUuid, double aX, double aY, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                modify( aTransaction, sheet, findItem<SCH_LABEL_BASE>( sheet, aUuid ),
                        [&]( SCH_LABEL_BASE* aLabel ) { aLabel->SetPosition( VECTOR2I( iu( aX ), iu( aY ) ) ); } );
                return done();
            },
            aResult, 0 );
}

int cc_sch_transaction_rename_label( cc_sch_transaction* aTransaction, const char* aPath,
                                     const char* aUuid, const char* aName, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                const wxString name = wxString::FromUTF8( required( aName, "label" ) );

                if( name.IsEmpty() )
                    throw ERROR( CC_SCH_INVALID, "Enter a net name" );

                modify( aTransaction, sheet, findItem<SCH_LABEL_BASE>( sheet, aUuid ),
                        [&]( SCH_LABEL_BASE* aLabel ) { aLabel->SetText( name ); } );
                return done();
            },
            aResult, 0 );
}

// API version 6: the schematic editor's tools below run on KiCad's model (CC_SCH_KICAD_TOOLS).

// SCH_EDIT_FRAME::AnnotateSymbols for the whole hierarchy with the Annotate dialog's defaults:
// sorted by X position, the first free number, multi-unit symbols kept together. Existing
// references stay unless aReset, which renumbers every symbol.
int cc_sch_transaction_annotate( cc_sch_transaction* aTransaction, int aReset, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                const cc_sch_session&        session = owner( aTransaction );
                SCH_SHEET_LIST               sheets = session.schematic->Hierarchy();
                SCH_REFERENCE_LIST           references;
                SCH_MULTI_UNIT_REFERENCE_MAP lockedSymbols;

                sheets.GetMultiUnitSymbols( lockedSymbols, SYMBOL_FILTER_ALL );
                sheets.GetSymbols( references, SYMBOL_FILTER_ALL );

                if( aReset )
                    references.RemoveAnnotation();

                references.SetRefDesTracker( session.schematic->Settings().m_refDesTracker );
                references.SplitReferences();
                references.AnnotateByOptions( SORT_BY_X_POSITION, INCREMENTAL_BY_REF, 0, lockedSymbols,
                                              SCH_REFERENCE_LIST(), false );

                for( size_t i = 0; i < references.GetCount(); ++i )
                {
                    SCH_REFERENCE& reference = references[i];
                    SCH_SHEET_PATH path = reference.GetSheetPath();
                    SCH_SYMBOL*    symbol = reference.GetSymbol();
                    wxString       before = symbol->GetRef( &path );

                    modify( aTransaction, path, symbol, [&]( SCH_SYMBOL* ) { reference.Annotate(); } );

                    if( symbol->GetRef( &path ) == before )
                    {
                        // Unchanged: drop the journal entry so an annotated sheet is not rewritten.
                        journal( aTransaction ).pop_back();
                    }
                }

                return done();
            },
            aResult, 0 );
}

// A bus segment, as the Draw Bus tool ends one (no merging).
int cc_sch_transaction_add_bus( cc_sch_transaction* aTransaction, const char* aPath, double aX0, double aY0,
                                double aX1, double aY1, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                const VECTOR2I start( iu( aX0 ), iu( aY0 ) ), end( iu( aX1 ), iu( aY1 ) );

                if( start == end )
                    throw ERROR( CC_SCH_INVALID, "A bus needs two different points" );

                auto* bus = new SCH_LINE( start, LAYER_BUS );
                bus->SetEndPoint( end );
                add( aTransaction, sheet, bus );
                addJunctions( aTransaction, sheet, { start, end } );
                return done();
            },
            aResult, 0 );
}

// KiCad's wire-to-bus entry (2.54 mm square) from (x, y) on the bus toward a quadrant:
// 1 up-right, 2 down-right, 3 down-left, 4 up-left (SCH_BUS_WIRE_ENTRY's quadrants).
int cc_sch_transaction_add_bus_entry( cc_sch_transaction* aTransaction, const char* aPath, double aX, double aY,
                                      int aQuadrant, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );

                if( aQuadrant < 1 || aQuadrant > 4 )
                    throw ERROR( CC_SCH_INVALID, "A bus entry quadrant is 1 to 4" );

                add( aTransaction, sheet, new SCH_BUS_WIRE_ENTRY( VECTOR2I( iu( aX ), iu( aY ) ), aQuadrant ) );
                return done();
            },
            aResult, 0 );
}

// A label of a scope: 0 local, 1 global, 2 hierarchical (bus names such as D[0..7] work in each).
int cc_sch_transaction_add_scoped_label( cc_sch_transaction* aTransaction, const char* aPath, const char* aName,
                                         double aX, double aY, int aKind, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                const wxString name = wxString::FromUTF8( required( aName, "label" ) );
                const VECTOR2I at( iu( aX ), iu( aY ) );

                if( name.IsEmpty() )
                    throw ERROR( CC_SCH_INVALID, "Enter a net name" );

                SCH_ITEM* label = aKind == 0   ? static_cast<SCH_ITEM*>( new SCH_LABEL( at, name ) )
                                  : aKind == 1 ? static_cast<SCH_ITEM*>( new SCH_GLOBALLABEL( at, name ) )
                                  : aKind == 2 ? static_cast<SCH_ITEM*>( new SCH_HIERLABEL( at, name ) )
                                               : nullptr;

                if( !label )
                    throw ERROR( CC_SCH_INVALID, "A label kind is 0 local, 1 global or 2 hierarchical" );

                add( aTransaction, sheet, label );
                return done();
            },
            aResult, 0 );
}

// Version 8. A no-connect flag (SCH_DRAWING_TOOLS' No Connect tool) at (x, y), normally a pin end.
int cc_sch_transaction_add_no_connect( cc_sch_transaction* aTransaction, const char* aPath, double aX, double aY,
                                       cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                add( aTransaction, sheet, new SCH_NO_CONNECT( VECTOR2I( iu( aX ), iu( aY ) ) ) );
                return done();
            },
            aResult, 0 );
}

// SCH_EDIT_TOOL::Mirror on one symbol: axis 0 flips it top to bottom (KiCad's Mirror Vertically,
// SYM_MIRROR_X), 1 left to right (Mirror Horizontally, SYM_MIRROR_Y), about its own origin.
int cc_sch_transaction_mirror_symbol( cc_sch_transaction* aTransaction, const char* aPath, const char* aUuid,
                                      int aAxis, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );

                if( aAxis != 0 && aAxis != 1 )
                    throw ERROR( CC_SCH_INVALID, "A mirror axis is 0 (X) or 1 (Y)" );

                modify( aTransaction, sheet, findItem<SCH_SYMBOL>( sheet, aUuid ),
                        [&]( SCH_SYMBOL* aSymbol )
                        {
                            aSymbol->SetOrientation( aAxis == 0 ? SYM_MIRROR_X : SYM_MIRROR_Y );
                            aSymbol->SetFieldsAutoplaced( AUTOPLACE_NONE );
                        } );
                return done();
            },
            aResult, 0 );
}

// A free text note (SCH_TEXT on the notes layer, KiCad's default size) anchored at (x, y).
int cc_sch_transaction_add_text( cc_sch_transaction* aTransaction, const char* aPath, const char* aText, double aX,
                                 double aY, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );
                const wxString text = wxString::FromUTF8( required( aText, "text" ) );

                if( wxString( text ).Trim().Trim( false ).IsEmpty() )
                    throw ERROR( CC_SCH_INVALID, "Enter some text" );

                add( aTransaction, sheet, new SCH_TEXT( VECTOR2I( iu( aX ), iu( aY ) ), text ) );
                return done();
            },
            aResult, 0 );
}

// A graphic line on the notes layer (saved as a two-point polyline, as KiCad's Line tool does).
int cc_sch_transaction_add_note_line( cc_sch_transaction* aTransaction, const char* aPath, double aX0, double aY0,
                                      double aX1, double aY1, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH  sheet = sheetPath( owner( aTransaction ), aPath );
                const VECTOR2I  start( iu( aX0 ), iu( aY0 ) ), end( iu( aX1 ), iu( aY1 ) );

                if( start == end )
                    throw ERROR( CC_SCH_INVALID, "A line needs two different ends" );

                SCH_LINE* line = new SCH_LINE( start, LAYER_NOTES );
                line->SetEndPoint( end );
                add( aTransaction, sheet, line );
                return done();
            },
            aResult, 0 );
}

// Version 9. A bus-to-bus entry (2.54 mm) from (x, y) toward quadrant 1 up-right, 2 down-right,
// 3 down-left or 4 up-left, sized as SCH_BUS_WIRE_ENTRY's quadrant constructor does.
int cc_sch_transaction_add_bus_bus_entry( cc_sch_transaction* aTransaction, const char* aPath, double aX, double aY,
                                          int aQuadrant, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheet = sheetPath( owner( aTransaction ), aPath );

                if( aQuadrant < 1 || aQuadrant > 4 )
                    throw ERROR( CC_SCH_INVALID, "A bus entry quadrant is 1 to 4" );

                auto*    entry = new SCH_BUS_BUS_ENTRY( VECTOR2I( iu( aX ), iu( aY ) ) );
                VECTOR2I size = entry->GetSize();
                size.x *= aQuadrant >= 3 ? -1 : 1;
                size.y *= aQuadrant == 1 || aQuadrant == 4 ? -1 : 1;
                entry->SetSize( size );
                add( aTransaction, sheet, entry );
                return done();
            },
            aResult, 0 );
}

// The point editor's sheet resize with the top-left corner kept: at least 5.08 mm and what its
// pins need (SCH_SHEET::GetMinWidth/GetMinHeight); SCH_SHEET::Resize keeps the pins on the edges.
int cc_sch_transaction_resize_sheet( cc_sch_transaction* aTransaction, const char* aPath, const char* aSheetUuid,
                                     double aWidth, double aHeight, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheetPathAt = sheetPath( owner( aTransaction ), aPath );
                SCH_SHEET*     sheet = findItem<SCH_SHEET>( sheetPathAt, aSheetUuid );
                const VECTOR2I size( iu( aWidth ), iu( aHeight ) );

                if( aWidth < 5.08 || aHeight < 5.08 || size.x < sheet->GetMinWidth( false )
                    || size.y < sheet->GetMinHeight( false ) )
                    throw ERROR( CC_SCH_INVALID, "The sheet must stay at least 5.08 mm and fit its pins" );

                modify( aTransaction, sheetPathAt, sheet, [&]( SCH_SHEET* aSheet ) { aSheet->Resize( size ); } );
                return done();
            },
            aResult, 0 );
}

// SCH_DRAWING_TOOLS::DrawSheet with a new file: a sheet box at (x, y) of the given size whose
// new schematic file <name>.kicad_sch (in the project folder) is written by the next save, with
// the next free page number.
int cc_sch_transaction_add_sheet( cc_sch_transaction* aTransaction, const char* aPath, const char* aName, double aX,
                                  double aY, double aWidth, double aHeight, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                const cc_sch_session& session = owner( aTransaction );
                SCH_SHEET_PATH        parent = sheetPath( session, aPath );
                const wxString        name = wxString::FromUTF8( required( aName, "sheet name" ) ).Trim().Trim( false );

                if( name.IsEmpty() || name.find_first_of( wxS( "/\\:*?\"<>|" ) ) != wxString::npos )
                    throw ERROR( CC_SCH_INVALID, "Use a sheet name without / \\ : * ? \" < > |" );

                if( aWidth < 5.08 || aHeight < 5.08 )
                    throw ERROR( CC_SCH_INVALID, "Draw the sheet at least 5.08 mm wide and tall" );

                const wxString file = name + wxS( "." ) + FILEEXT::KiCadSchematicFileExtension;
                const fs::path absolute = session.folder / fs::path( utf8( file ) );

                for( const SCH_SHEET_PATH& sheet : session.schematic->Hierarchy() )
                {
                    if( sheet.size() > 1 && sheet.Last()->GetName() == name )
                        throw ERROR( CC_SCH_CONFLICT, "A sheet named " + utf8( name ) + " exists" );
                }

                if( fs::exists( absolute ) )
                    throw ERROR( CC_SCH_CONFLICT, "The project already has " + utf8( file ) );

                auto* sheet = new SCH_SHEET( parent.Last(), VECTOR2I( iu( aX ), iu( aY ) ),
                                             VECTOR2I( iu( aWidth ), iu( aHeight ) ) );
                auto* screen = new SCH_SCREEN( session.schematic.get() );
                screen->SetFileName( wxString::FromUTF8( absolute.string() ) );
                sheet->SetScreen( screen );
                sheet->GetField( FIELD_T::SHEET_NAME )->SetText( name );
                sheet->GetField( FIELD_T::SHEET_FILENAME )->SetText( file );
                sheet->AutoplaceFields( parent.LastScreen(), AUTOPLACE_AUTO );
                add( aTransaction, parent, sheet );
                session.schematic->RefreshHierarchy();

                std::set<long> used;

                for( const SCH_SHEET_PATH& path : session.schematic->Hierarchy() )
                {
                    long number = 0;

                    if( path.GetPageNumber().ToLong( &number ) && number > 0 )
                        used.insert( number );
                }

                long next = 1;

                while( used.count( next ) )
                    ++next;

                SCH_SHEET_PATH instance = parent;
                instance.push_back( sheet );
                instance.SetPageNumber( wxString::Format( wxS( "%ld" ), next ) );
                return done();
            },
            aResult, 0 );
}

// The sheet tool's Import Sheet Pin for every hierarchical label of the child sheet that has no
// pin yet: on the left edge, every 2.54 mm below the existing pins, with the label's shape.
int cc_sch_transaction_add_sheet_pins( cc_sch_transaction* aTransaction, const char* aPath, const char* aSheetUuid,
                                       cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                SCH_SHEET_PATH sheetPathAt = sheetPath( owner( aTransaction ), aPath );
                SCH_SHEET*     sheet = findItem<SCH_SHEET>( sheetPathAt, aSheetUuid );
                std::vector<SCH_HIERLABEL*> missing;

                for( SCH_ITEM* item : sheet->GetScreen()->Items().OfType( SCH_HIER_LABEL_T ) )
                {
                    auto* label = static_cast<SCH_HIERLABEL*>( item );

                    if( !sheet->HasPin( label->GetText() ) )
                        missing.push_back( label );
                }

                if( missing.empty() )
                    throw ERROR( CC_SCH_NOT_FOUND, "Every hierarchical label of the sheet has a pin" );

                const int step = iu( 2.54 );
                int       y = sheet->GetPosition().y + step;

                for( SCH_SHEET_PIN* pin : sheet->GetPins() )
                    y = std::max( y, pin->GetPosition().y + step );

                if( y + step * int( missing.size() - 1 ) > sheet->GetPosition().y + sheet->GetSize().y - step / 2 )
                    throw ERROR( CC_SCH_INVALID, "Make the sheet taller for its pins" );

                modify( aTransaction, sheetPathAt, sheet,
                        [&]( SCH_SHEET* aSheet )
                        {
                            for( SCH_HIERLABEL* label : missing )
                            {
                                auto* pin = new SCH_SHEET_PIN( aSheet, VECTOR2I( aSheet->GetPosition().x, y ),
                                                               label->GetText() );
                                pin->SetShape( label->GetShape() );
                                pin->SetSide( SHEET_SIDE::LEFT );
                                pin->SetPosition( VECTOR2I( aSheet->GetPosition().x, y ) );
                                aSheet->AddPin( pin );
                                y += step;
                            }
                        } );
                return done();
            },
            aResult, 0 );
}

int cc_sch_transaction_commit( cc_sch_transaction* aTransaction, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                JOURNAL&        changes = journal( aTransaction );
                cc_sch_session& session = *aTransaction->session;

                if( !changes.empty() )
                {
                    for( CHANGE& change : changes )
                        session.dirty.insert( change.screen );

                    session.history.resize( session.index );
                    session.history.push_back( std::move( changes ) );
                    session.index = session.history.size();

                    // Same depth as the compatible backend.
                    if( session.history.size() > 50 )
                    {
                        session.history.erase( session.history.begin() );
                        --session.index;
                    }

                    connect( session );
                }

                aTransaction->active = false;
                session.open = false;
                return done();
            },
            aResult, 0 );
}

void cc_sch_transaction_free( cc_sch_transaction* aTransaction )
{
    if( !aTransaction )
        return;

    if( aTransaction->active )
    {
        for( auto it = aTransaction->changes.rbegin(); it != aTransaction->changes.rend(); ++it )
            flip( *it );

        aTransaction->session->open = false;

        // ponytail: a destructor-like call cannot report errors, so a failure here is dropped.
        if( !aTransaction->changes.empty() )
        {
            try
            {
                connect( *aTransaction->session );
            }
            catch( ... )
            {
            }
        }
    }

    delete aTransaction;
}

int cc_sch_session_can_undo( const cc_sch_session* aSession )
{
    return aSession && !aSession->open && aSession->index > 0 ? 1 : 0;
}

int cc_sch_session_can_redo( const cc_sch_session* aSession )
{
    return aSession && !aSession->open && aSession->index < aSession->history.size() ? 1 : 0;
}

char* cc_sch_session_sheet_source( const cc_sch_session* aSession, const char* aPath,
                                   cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                if( !aSession )
                    throw ERROR( CC_SCH_INVALID, "Missing schematic session" );

                SCH_SCREEN* screen = sheetPath( *aSession, aPath ).LastScreen();
                return owned( throughFile( ".kicad_sch", [&]( const wxString& aFile )
                        { writeScreen( const_cast<cc_sch_session&>( *aSession ), screen,
                                       fs::path( utf8( aFile ) ) ); } ) );
            },
            aResult, static_cast<char*>( nullptr ) );
}

size_t cc_sch_session_route( const cc_sch_session* aSession, const char* aPath, double aX0,
                             double aY0, double aX1, double aY1, double* aXY, size_t aCapacity,
                             cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                if( !aSession )
                    throw ERROR( CC_SCH_INVALID, "Missing schematic session" );

                const auto route = corvcad::plan_orthogonal_route( obstacles( sheetPath( *aSession, aPath ) ),
                                                                   aX0, aY0, aX1, aY1 );

                for( size_t i = 0; i < route.size() && i < aCapacity && aXY; ++i )
                {
                    aXY[2 * i] = route[i].x;
                    aXY[2 * i + 1] = route[i].y;
                }

                return route.size();
            },
            aResult, size_t( 0 ) );
}

int cc_sch_session_undo( cc_sch_session* aSession, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                requireIdle( aSession );

                if( aSession->index == 0 )
                    throw ERROR( CC_SCH_INVALID, "Nothing to undo" );

                finish( *aSession, aSession->history[--aSession->index], false );
                return done();
            },
            aResult, 0 );
}

int cc_sch_session_redo( cc_sch_session* aSession, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                requireIdle( aSession );

                if( aSession->index == aSession->history.size() )
                    throw ERROR( CC_SCH_INVALID, "Nothing to redo" );

                finish( *aSession, aSession->history[aSession->index++], true );
                return done();
            },
            aResult, 0 );
}

// KiCad loaded every sheet, so there is nothing structural left to reject.
int cc_sch_session_validate_structure( cc_sch_session* aSession, cc_sch_result* aResult )
{
    return invoke( [&] { requireIdle( aSession ); return done(); }, aResult, 0 );
}

int cc_sch_session_save( cc_sch_session* aSession, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                requireIdle( aSession );
                std::vector<fs::path> staged;

                // A sheet added in this session has a screen whose file does not exist yet; one
                // undone is out of the hierarchy and is not written.
                std::set<SCH_SCREEN*> present;

                for( const SCH_SHEET_PATH& sheet : aSession->schematic->Hierarchy() )
                {
                    present.insert( sheet.LastScreen() );

                    if( !fs::exists( fs::path( utf8( sheet.LastScreen()->GetFileName() ) ) ) )
                        aSession->dirty.insert( sheet.LastScreen() );
                }

                std::erase_if( aSession->dirty, [&]( SCH_SCREEN* aScreen ) { return !present.count( aScreen ); } );

                try
                {
                    for( SCH_SCREEN* screen : aSession->dirty )
                    {
                        const fs::path file( utf8( screen->GetFileName() ) );
                        const auto     known = aSession->loaded.find( screen );

                        if( known != aSession->loaded.end() && fs::exists( file )
                            && fs::last_write_time( file ) != known->second )
                            throw ERROR( CC_SCH_CONFLICT,
                                         "Schematic changed on disk; reopen before saving: " + file.string() );

                        staged.push_back( file.string() + ".corvcad.tmp" );
                        writeScreen( *aSession, screen, staged.back() );
                    }
                }
                catch( ... )
                {
                    std::error_code ignored;

                    for( const fs::path& tmp : staged )
                        fs::remove( tmp, ignored );

                    throw;
                }

                // ponytail: each rename is atomic, but a failure between two renames leaves
                // the project half saved; add the compatible backend's backups if that matters.
                for( SCH_SCREEN* screen : aSession->dirty )
                {
                    const fs::path file( utf8( screen->GetFileName() ) );
                    fs::rename( file.string() + ".corvcad.tmp", file );
                    aSession->loaded[screen] = fs::last_write_time( file );
                }

                aSession->dirty.clear();
                return done();
            },
            aResult, 0 );
}

int cc_sch_session_save_copy( cc_sch_session* aSession, const char* aDirectory, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                requireIdle( aSession );
                const fs::path target = fs::weakly_canonical( fs::path( required( aDirectory, "destination" ) ) );
                const fs::path relative = target.lexically_relative( aSession->folder );

                if( fs::exists( target ) || target.filename().empty()
                    || ( !relative.empty() && *relative.begin() != ".." ) )
                    throw ERROR( CC_SCH_INVALID, "Choose a new directory outside the source project" );

                const fs::path staged = target.string() + ".corvcad.tmp";

                try
                {
                    fs::copy( aSession->folder, staged, fs::copy_options::recursive );
                    std::set<SCH_SCREEN*> written;

                    for( const SCH_SHEET_PATH& sheet : aSession->schematic->Hierarchy() )
                    {
                        if( written.insert( sheet.LastScreen() ).second )
                            writeScreen( *aSession, sheet.LastScreen(),
                                         staged / fs::path( relativeFile( *aSession, sheet.LastScreen() ) ) );
                    }

                    fs::rename( staged, target );
                }
                catch( ... )
                {
                    std::error_code ignored;
                    fs::remove_all( staged, ignored );
                    throw;
                }

                return done();
            },
            aResult, 0 );
}

char* cc_sch_session_netlist( cc_sch_session* aSession, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                requireIdle( aSession );
                NETLIST_EXPORTER_KICAD exporter( aSession->schematic.get() );
                STRING_FORMATTER       formatter;
                exporter.Format( &formatter, GNL_ALL | GNL_OPT_KICAD );
                return owned( formatter.GetString() );
            },
            aResult, static_cast<char*>( nullptr ) );
}

#ifdef CORVCAD_NGSPICE_STATIC
namespace
{
// KiCad's NGSPICE reports its background thread starting (running) and ending (idle), and idle
// too when ngspice calls its controlled exit (an error that may end the thread before it says
// so). ngSpice_running() is true only once the thread has started, which can be after bg_run
// returns, so the wait needs the start as well.
struct SIM_RUN_LISTENER : SIM_STATE_LISTENER
{
    std::atomic<bool> started{ false };
    std::atomic<bool> idle{ false };
    void OnSimStateChange( SIMULATOR*, SIM_STATE aState ) override
    {
        if( aState == SIM_RUNNING )
            started = true;
        else if( aState == SIM_IDLE )
            idle = true;
    }
};

// One simulation's netlist input: KiCad's SPICE model of the session's current state, the
// analysis command and the netlist options (cc_sch_session_simulate's `command`).
struct SIM_REQUEST
{
    std::shared_ptr<SPICE_CIRCUIT_MODEL> model;
    wxString                             command;
    unsigned                             options = NETLIST_EXPORTER_SPICE::OPTION_DEFAULT_FLAGS;
};

SIM_REQUEST simRequest( cc_sch_session& aSession, const char* aCommand, REPORTER& aReporter )
{
    SIM_REQUEST request;
    aSession.schematic->RecalculateConnections( nullptr, GLOBAL_CLEANUP, nullptr );
    request.model = std::make_shared<SPICE_CIRCUIT_MODEL>( aSession.schematic.get() );
    // A Simulator tab's options, as its workbook stores them among the commands
    // (SIMULATOR_FRAME_UI's workbook reading): with any of these lines, the netlist options are
    // exactly the ones listed; without, a new tab's defaults.
    unsigned listed = NETLIST_EXPORTER_SPICE::OPTION_ADJUST_PASSIVE_VALS
                      | NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_EVENTS;

    for( wxString line : wxSplit( wxString::FromUTF8( aCommand ? aCommand : "" ), '\n', '\0' ) )
    {
        line.Trim().Trim( false );

        if( line == wxS( ".kicad adjustpaths" ) )
            listed |= NETLIST_EXPORTER_SPICE::OPTION_ADJUST_INCLUDE_PATHS;
        else if( line == wxS( ".save all" ) )
            listed |= NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_VOLTAGES;
        else if( line == wxS( ".probe alli" ) )
            listed |= NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_CURRENTS;
        else if( line == wxS( ".probe allp" ) )
            listed |= NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_DISSIPATIONS;
        else if( line == wxS( ".kicad esavenone" ) )
            listed &= ~NETLIST_EXPORTER_SPICE::OPTION_SAVE_ALL_EVENTS;
        else
        {
            if( !line.IsEmpty() )
                request.command += ( request.command.IsEmpty() ? wxString() : wxString( wxS( "\n" ) ) ) + line;

            continue;
        }

        request.options = listed;
    }

    if( request.command.IsEmpty() )
    {
        request.model->ReadSchematicAndLibraries( request.options, aReporter );
        request.command = request.model->GetSchTextSimCommand();
    }

    if( request.command.IsEmpty() )
        throw ERROR( CC_SCH_INVALID, "no simulation command: pass one, or add a directive to the schematic" );

    return request;
}
} // namespace
#endif

// Stops the run in progress, from any thread; cc_sch_session_simulate then returns what it has.
int cc_sch_simulate_stop( void )
{
#ifdef CORVCAD_NGSPICE_STATIC
    std::shared_ptr<SPICE_SIMULATOR> simulator =
            std::dynamic_pointer_cast<SPICE_SIMULATOR>( SIMULATOR::CreateInstance( "ngspice" ) );
    return simulator && simulator->IsRunning() && simulator->Stop() ? 1 : 0;
#else
    return 0;
#endif
}

// KiCad's SPICE netlist for one simulation, the text KiCad's NGSPICE loads (NGSPICE::Attach):
// the same `command` handling as cc_sch_session_simulate.
char* cc_sch_session_spice_netlist( cc_sch_session* aSession, const char* aCommand, cc_sch_result* aResult )
{
    return invoke(
            [&]() -> char*
            {
                requireIdle( aSession );
#ifndef CORVCAD_NGSPICE_STATIC
                throw ERROR( CC_SCH_UNSUPPORTED, "this build has no simulator" );
#else
                WX_STRING_REPORTER reporter;
                SIM_REQUEST        request = simRequest( *aSession, aCommand, reporter );
                STRING_FORMATTER   formatter;

                if( !request.model->GetNetlist( request.command, request.options, &formatter, reporter ) )
                    throw ERROR( CC_SCH_INVALID, utf8( wxString( reporter.GetMessages() ).Trim() ) );

                return owned( formatter.GetString() );
#endif
            },
            aResult, static_cast<char*>( nullptr ) );
}

// KiCad's simulator as its Simulator window runs one analysis (SIMULATOR_FRAME::StartSimulation
// and LoadSimulator): the project's ngspice settings, KiCad's SPICE netlist of the current state,
// then ngspice in its background thread.
char* cc_sch_session_simulate( cc_sch_session* aSession, const char* aCommand, size_t aMaxPoints,
                               cc_sch_result* aResult )
{
    return invoke(
            [&]() -> char*
            {
                requireIdle( aSession );
#ifndef CORVCAD_NGSPICE_STATIC
                throw ERROR( CC_SCH_UNSUPPORTED, "this build has no simulator" );
#else
                std::shared_ptr<SPICE_SIMULATOR> simulator =
                        std::dynamic_pointer_cast<SPICE_SIMULATOR>( SIMULATOR::CreateInstance( "ngspice" ) );

                if( !simulator )
                    throw ERROR( CC_SCH_UNSUPPORTED, "ngspice did not start" );

                // ngspice keeps global state: one simulation at a time in the process.
                std::unique_lock<std::mutex> busy( simulator->GetMutex(), std::try_to_lock );

                if( !busy.owns_lock() )
                    throw ERROR( CC_SCH_CONFLICT, "another simulation is running" );

                std::shared_ptr<NGSPICE_SETTINGS> settings =
                        aSession->schematic->Project().GetProjectFile().m_SchematicSettings->m_NgspiceSettings;

                // As the Simulator window does for a project without a workbook.
                if( settings->GetWorkbookFilename().IsEmpty() )
                    settings->SetCompatibilityMode( NGSPICE_COMPATIBILITY_MODE::LT_PSPICE );

                simulator->Settings() = settings;
                simulator->Init();

                WX_STRING_REPORTER console;
                SIM_RUN_LISTENER   listener;
                simulator->SetReporter( &console );
                simulator->SetSimStateListener( &listener );

                struct DETACH
                {
                    SPICE_SIMULATOR& sim;
                    ~DETACH()
                    {
                        sim.SetSimStateListener( nullptr );
                        sim.SetReporter( nullptr );
                    }
                } detach{ *simulator };

                WX_STRING_REPORTER netlist;
                SIM_REQUEST        request = simRequest( *aSession, aCommand, netlist );
                const wxString&    command = request.command;

                if( !simulator->Attach( request.model, command, request.options,
                                        wxString::FromUTF8( aSession->folder.string() + "/" ), netlist ) )
                {
                    throw ERROR( CC_SCH_INVALID, utf8( wxString( netlist.GetMessages() ).Trim() ) );
                }

                // Loading the netlist can report idle too; only reports after Run count.
                listener.started = false;
                listener.idle = false;

                if( !simulator->Run() )
                    throw ERROR( CC_SCH_INVALID, "ngspice did not accept the run" );

                const auto launched = std::chrono::steady_clock::now();

                while( !listener.started && !listener.idle )
                {
                    if( std::chrono::steady_clock::now() - launched > std::chrono::seconds( 30 ) )
                        throw ERROR( CC_SCH_IO, "ngspice's simulation thread did not start" );

                    std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                }

                while( !listener.idle && simulator->IsRunning() )
                    std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );

                const std::string plot = utf8( simulator->CurrentPlotName() );
                std::vector<std::string> vectors = simulator->AllVectors();

                std::sort( vectors.begin(), vectors.end() );
                std::ostringstream out;
                out.imbue( std::locale::classic() );
                out << std::setprecision( 17 );
                size_t points = 0;
                std::vector<std::pair<std::vector<double>, std::vector<double>>> data;

                for( const std::string& name : vectors )
                {
                    data.emplace_back( simulator->GetRealVector( name ), simulator->GetImaginaryVector( name ) );
                    points = std::max( points, data.back().first.size() );
                }

                // ngspice's own plot before any run is "const"; a failed run adds no new plot, and
                // one refused at the start (too large for memory) has vectors with no points.
                if( points == 0 || plot == "const" )
                {
                    std::string output = utf8( wxString( console.GetMessages() ).Trim() );
                    simulator->Clean();
                    throw ERROR( CC_SCH_INVALID, "the simulation produced no results"
                                                         + ( output.empty() ? std::string() : ":\n" + output ) );
                }

                // Every stride-th point, so that about aMaxPoints remain (0: all), and the last.
                const size_t stride = aMaxPoints && points > aMaxPoints ? ( points + aMaxPoints - 1 ) / aMaxPoints : 1;
                std::vector<size_t> kept;

                for( size_t i = 0; i < points; i += stride )
                    kept.push_back( i );

                if( !kept.empty() && kept.back() != points - 1 )
                    kept.push_back( points - 1 );

                out << utf8( SPICE_SIMULATOR::TypeToName( SPICE_CIRCUIT_MODEL::CommandToSimType( command ), true ) )
                    << '\t' << plot << '\t' << points << '\t' << kept.size() << '\n';

                for( size_t v = 0; v < vectors.size(); ++v )
                {
                    const std::vector<double>& re = data[v].first;
                    const std::vector<double>& im = data[v].second;
                    out << vectors[v] << '\t' << ( im.empty() ? "real" : "complex" ) << '\t';
                    bool first = true;

                    for( size_t i : kept )
                    {
                        if( i >= re.size() )
                            break;

                        out << ( first ? "" : " " ) << re[i];
                        first = false;

                        if( !im.empty() )
                            out << ',' << ( i < im.size() ? im[i] : 0.0 );
                    }

                    out << '\n';
                }

                // The results are in the text now; free ngspice's copy (KiCad destroys a tab's
                // plot before each rerun).
                simulator->Clean();
                return owned( out.str() );
#endif
            },
            aResult, static_cast<char*>( nullptr ) );
}

// The bill of materials as `kicad-cli sch export bom --group-by Value,Footprint
// --ref-range-delimiter -` writes it (columns Refs, Value, Footprint, Qty, DNP; rows grouped by
// Value and Footprint, sorted by reference; symbols excluded from the BOM left out). This
// follows FIELDS_EDITOR_GRID_DATA_MODEL's grouping and Export, which the app does not link
// (it is the editor's grid table).
char* cc_sch_session_bom_csv( cc_sch_session* aSession, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                requireIdle( aSession );
                SCH_REFERENCE_LIST references;
                aSession->schematic->Hierarchy().GetSymbols( references, SYMBOL_FILTER_NON_POWER, false );
                references.SplitReferences(); // GetRef/GetRefNumber as the data model reads them

                auto shown = [&]( const SCH_REFERENCE& aRef, FIELD_T aField )
                {
                    return aRef.GetSymbol()->GetField( aField )->GetShownText( &aRef.GetSheetPath(), false, 0,
                                                                               wxEmptyString );
                };
                auto raw = [&]( const SCH_REFERENCE& aRef, FIELD_T aField )
                {
                    return aRef.GetSymbol()->GetField( aField )->GetText();
                };
                auto unitMatch = [&]( const SCH_REFERENCE& a, const SCH_REFERENCE& b )
                {
                    return a.GetRefNumber() != wxT( "?" ) && a.GetRef() == b.GetRef()
                           && a.GetRefNumber() == b.GetRefNumber();
                };

                std::vector<std::vector<SCH_REFERENCE>> rows;

                for( size_t i = 0; i < references.GetCount(); ++i )
                {
                    const SCH_REFERENCE& ref = references[i];

                    if( ref.GetSymbol()->ResolveExcludedFromBOM( &ref.GetSheetPath(), wxEmptyString )
                        || ref.GetSheetPath().GetExcludedFromBOM( wxEmptyString ) )
                        continue;

                    auto row = std::find_if( rows.begin(), rows.end(), [&]( const auto& aRow )
                    {
                        const SCH_REFERENCE& first = aRow.front();
                        return unitMatch( ref, first )
                               || ( raw( ref, FIELD_T::VALUE ) == raw( first, FIELD_T::VALUE )
                                    && raw( ref, FIELD_T::FOOTPRINT ) == raw( first, FIELD_T::FOOTPRINT ) );
                    } );

                    if( row == rows.end() )
                        rows.push_back( { ref } );
                    else
                        row->push_back( ref );
                }

                auto fullRef = []( const SCH_REFERENCE& aRef ) { return aRef.GetRef() + aRef.GetRefNumber(); };
                auto byRef = [&]( const SCH_REFERENCE& a, const SCH_REFERENCE& b )
                { return StrNumCmp( fullRef( a ), fullRef( b ), true ) < 0; };

                for( auto& row : rows )
                    std::sort( row.begin(), row.end(), byRef );

                std::sort( rows.begin(), rows.end(),
                           [&]( const auto& a, const auto& b ) { return byRef( a.front(), b.front() ); } );

                auto cell = []( wxString aText, bool aLast )
                {
                    aText.Replace( wxS( "\r" ), wxS( "" ) );
                    aText.Replace( wxS( "\n" ), wxS( "" ) );
                    aText.Replace( wxS( "\t" ), wxS( "" ) );
                    aText.Replace( wxS( "\"" ), wxS( "\"\"" ) );
                    return wxS( "\"" ) + aText + wxS( "\"" ) + ( aLast ? wxS( "\n" ) : wxS( "," ) );
                };

                wxString out = cell( wxS( "Refs" ), false ) + cell( wxS( "Value" ), false )
                               + cell( wxS( "Footprint" ), false ) + cell( wxS( "Qty" ), false )
                               + cell( wxS( "DNP" ), true );

                for( const auto& row : rows )
                {
                    // Other units of a multi-unit part count once.
                    std::vector<SCH_REFERENCE> unique( row );
                    unique.erase( std::unique( unique.begin(), unique.end(),
                                               [&]( const SCH_REFERENCE& a, const SCH_REFERENCE& b )
                                               { return a.GetRefNumber() != wxT( "?" ) && fullRef( a ) == fullRef( b ); } ),
                                  unique.end() );

                    auto same = [&]( FIELD_T aField )
                    {
                        wxString value = shown( row.front(), aField );

                        for( const SCH_REFERENCE& ref : row )
                        {
                            if( shown( ref, aField ) != value )
                                return wxString( wxS( "-- mixed values --" ) ); // INDETERMINATE_STATE
                        }

                        return value;
                    };

                    const SCH_REFERENCE& first = row.front();
                    wxString             dnp = wxS( "${DNP}" );
                    std::function<bool( wxString* )> resolver = [&]( wxString* aToken )
                    { return first.GetSymbol()->ResolveTextVar( &first.GetSheetPath(), aToken, wxEmptyString ); };

                    out += cell( SCH_REFERENCE_LIST::Shorthand( unique, wxS( "," ), wxS( "-" ) ), false )
                           + cell( same( FIELD_T::VALUE ), false ) + cell( same( FIELD_T::FOOTPRINT ), false )
                           + cell( wxString::Format( wxT( "%d" ), (int) unique.size() ), false )
                           + cell( ExpandTextVars( dnp, &resolver ), true );
                }

                return owned( utf8( out ) );
            },
            aResult, static_cast<char*>( nullptr ) );
}

// kicad-cli's JobSchErc with its defaults, as probe/probe.cpp runs it, including the footprint
// link check when the board code is linked (CorvCadFootprintLinkKiface).
char* cc_sch_session_erc_json( cc_sch_session* aSession, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                requireIdle( aSession );
                SCHEMATIC*  schematic = aSession->schematic.get();
                SCH_SCREEN* root = schematic->RootScreen();
                SCH_SCREENS screens( schematic->Root() );
                // Only the libraries the schematic's symbols refer to. kicad-cli loads every
                // library of the tables (AsyncLoad), which on an iPad holds about 1 GB for the
                // 223 official ones; ERC only looks up the referenced libraries.
                SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &schematic->Project() );
                std::set<wxString>      nicknames;

                for( const SCH_SHEET_PATH& sheet : schematic->Hierarchy() )
                {
                    for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
                        nicknames.insert( wxString( static_cast<SCH_SYMBOL*>( item )->GetLibId().GetLibNickname() ) );
                }

                for( const wxString& nickname : nicknames )
                {
                    if( !nickname.IsEmpty() )
                        adapter->LoadOne( nickname );
                }

                DS_PROXY_VIEW_ITEM drawingSheet( schIUScale, &root->GetPageSettings(), &schematic->Project(),
                                                 &root->GetTitleBlock(), schematic->GetProperties() );
                drawingSheet.SetPageNumber( utf8( root->GetPageNumber() ) );
                drawingSheet.SetSheetCount( root->GetPageCount() );
                drawingSheet.SetFileName( utf8( root->GetFileName() ) );
                drawingSheet.SetIsFirstPage( root->GetVirtualPageNumber() == 1 );
                drawingSheet.SetVariantName( utf8( schematic->GetCurrentVariant() ) );
                drawingSheet.SetVariantDesc( utf8( schematic->GetVariantDescription( schematic->GetCurrentVariant() ) ) );
                drawingSheet.SetSheetName( "" );
                drawingSheet.SetSheetPath( "" );

                // Markers of an earlier run would be reported twice; remove them after too, so
                // screens hold only the user's items.
                screens.DeleteAllMarkers( MARKER_BASE::MARKER_ERC, false );
                ERC_TESTER( schematic ).RunTests( &drawingSheet, nullptr, CorvCadFootprintLinkKiface(),
                                                  &schematic->Project(), nullptr );
                auto markers = std::make_shared<SHEETLIST_ERC_ITEMS_PROVIDER>( schematic );
                markers->SetSeverities( RPT_SEVERITY_ERROR | RPT_SEVERITY_WARNING );
                std::string report = throughFile( ".json", [&]( const wxString& aFile )
                        { ERC_REPORT( schematic, EDA_UNITS::MM, markers ).WriteJsonReport( aFile ); } );
                screens.DeleteAllMarkers( MARKER_BASE::MARKER_ERC, false );
                return owned( report );
            },
            aResult, static_cast<char*>( nullptr ) );
}

// KiCad's schematic plot as `kicad-cli sch export pdf|svg` runs it with its defaults
// (EESCHEMA_JOBS_HANDLER::JobExportPlot and InitRenderSettings): every sheet with its drawing
// sheet and background, the default theme, KiCad's font, a 847 nm minimum pen; PDF with
// property popups, hierarchical links and metadata. Format 0 writes one PDF at aOutput,
// format 1 one SVG per sheet into the directory aOutput.
int cc_sch_session_plot( cc_sch_session* aSession, int aFormat, const char* aOutput, cc_sch_result* aResult )
{
    return invoke(
            [&]
            {
                requireIdle( aSession );

                if( ( aFormat != 0 && aFormat != 1 ) || !aOutput || !*aOutput )
                    throw std::invalid_argument( "Plot format 0 (PDF file) or 1 (SVG directory) and an output path" );

                SCHEMATIC*          schematic = aSession->schematic.get();
                SCH_RENDER_SETTINGS settings;
                settings.LoadColors( ::GetColorSettings( wxEmptyString ) );
                settings.m_ShowHiddenPins = false;
                settings.m_ShowHiddenFields = false;
                settings.m_ShowPinAltIcons = false;
                settings.SetDefaultPenWidth( schematic->Settings().m_DefaultLineWidth );
                settings.m_LabelSizeRatio = schematic->Settings().m_LabelSizeRatio;
                settings.m_TextOffsetRatio = schematic->Settings().m_TextOffsetRatio;
                settings.m_PinSymbolSize = schematic->Settings().m_PinSymbolSize;
                settings.SetDashLengthRatio( schematic->Settings().m_DashedLineDashRatio );
                settings.SetGapLengthRatio( schematic->Settings().m_DashedLineGapRatio );
                settings.SetDefaultFont( KICAD_FONT_NAME );
                settings.SetMinPenWidth( 847 );

                // The project's drawing sheet, else KiCad's default one, as kicad-cli loads it.
                wxString  message;
                wxString  sheetFile = ExpandEnvVarSubstitutions( schematic->Settings().m_SchDrawingSheetFileName,
                                                                 &schematic->Project() );
                wxFileName sheetPath( sheetFile );

                if( !sheetFile.IsEmpty() && sheetPath.IsRelative() )
                    sheetPath.MakeAbsolute( schematic->Project().GetProjectPath() );

                if( sheetFile.IsEmpty()
                    || !DS_DATA_MODEL::GetTheInstance().LoadDrawingSheet( sheetPath.GetFullPath(), &message ) )
                    DS_DATA_MODEL::GetTheInstance().LoadDrawingSheet( wxEmptyString, &message );

                // Text boxes cached while loading used no default font yet (as kicad-cli clears).
                SCH_SCREENS screens( schematic->Root() );

                for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
                {
                    for( SCH_ITEM* item : screen->Items() )
                        item->ClearCaches();

                    for( const auto& [name, symbol] : screen->GetLibSymbols() )
                        symbol->ClearCaches();
                }

                SCH_PLOT_OPTS options;
                options.m_PDFPropertyPopups = true;
                options.m_PDFHierarchicalLinks = true;
                options.m_PDFMetadata = true;
                options.m_pageSizeSelect = PageFormatReq::PAGE_SIZE_AUTO;

                if( aFormat == 0 )
                    options.m_outputFile = wxString::FromUTF8( aOutput );
                else
                    options.m_outputDirectory = wxString::FromUTF8( aOutput );

                WX_STRING_REPORTER reporter;
                SCH_PLOTTER( schematic ).Plot( aFormat == 0 ? PLOT_FORMAT::PDF : PLOT_FORMAT::SVG, options,
                                               &settings, &reporter );

                if( reporter.HasMessageOfSeverity( RPT_SEVERITY_ERROR ) )
                    throw std::runtime_error( utf8( reporter.GetMessages() ) );

                return 1;
            },
            aResult, 0 );
}

// Version 10. KiCad's schematic import as SCH_EDIT_FRAME::importFile runs it, without the frame:
// the format's SCH_IO plugin loads `input` into a new schematic of the project that owns
// `output` (its .kicad_pro must exist and no session may be open), footprint fields are
// re-linked to the import's footprint cache library (SCH_FOOTPRINT_FIELD_RECONCILER, the
// nickname cc_board_native_import uses), the drawing sheet is emptied as for any non-KiCad
// schematic, connections are recalculated with KiCad's cleanup, and every sheet is saved as a
// KiCad file, the root at `output`. Format: auto, altium, eagle, easyeda, easyedapro, cadstar,
// pads, ltspice or geda. Returns "<format>\t<sheets>\t<symbols>\t<importer messages>".
char* cc_sch_import( const char* aInput, const char* aFormat, const char* aOutput, cc_sch_result* aResult )
{
    return invoke(
            [&]() -> char*
            {
                const wxString input = wxString::FromUTF8( required( aInput, "input file" ) );
                const fs::path output = fs::absolute( fs::path( required( aOutput, "output schematic" ) ) );
                const std::string format = aFormat ? aFormat : "auto";
                static const std::map<std::string, SCH_IO_MGR::SCH_FILE_T> formats = {
                    { "altium", SCH_IO_MGR::SCH_ALTIUM },   { "eagle", SCH_IO_MGR::SCH_EAGLE },
                    { "easyeda", SCH_IO_MGR::SCH_EASYEDA }, { "easyedapro", SCH_IO_MGR::SCH_EASYEDAPRO },
                    { "cadstar", SCH_IO_MGR::SCH_CADSTAR_ARCHIVE }, { "pads", SCH_IO_MGR::SCH_PADS },
                    { "ltspice", SCH_IO_MGR::SCH_LTSPICE }, { "geda", SCH_IO_MGR::SCH_GEDA } };
                SCH_IO_MGR::SCH_FILE_T type = format == "auto"
                        ? SCH_IO_MGR::GuessPluginTypeFromSchPath( input, 1 << 0 /* KICTL_NONKICAD_ONLY, kiway_player.h */ )
                        : formats.count( format ) ? formats.at( format ) : SCH_IO_MGR::SCH_FILE_UNKNOWN;
                bool importable = false;

                for( const auto& [name, known] : formats )
                    importable |= known == type;

                if( !importable )
                    throw ERROR( CC_SCH_INVALID, "Not a schematic format KiCad imports" );

                if( output.extension() != ".kicad_sch" )
                    throw ERROR( CC_SCH_INVALID, "Import to a .kicad_sch path" );

                fs::path projectFile = output;
                projectFile.replace_extension( ".kicad_pro" );

                if( !fs::is_regular_file( projectFile ) )
                    throw ERROR( CC_SCH_INVALID, "Create the project first: " + projectFile.string() );

                startKiCad();
                const wxString file = wxString::FromUTF8( output.string() );

                if( g_liveSessions > 0 )
                    throw ERROR( CC_SCH_CONFLICT, "Close the open schematic before importing" );

                CorvCadActivateProject( file );

                SETTINGS_MANAGER& manager = Pgm().GetSettingsManager();
                manager.LoadProject( wxString::FromUTF8( projectFile.string() ), true );
                PROJECT&           project = manager.Prj();
                const wxString     cache = IMPORT_PROJ_PROPS::MakeCacheNickname( project.GetProjectName() );
                std::map<std::string, UTF8> properties;
                properties[IMPORT_PROJ_PROPS::FP_CACHE_NICKNAME] = UTF8( cache );

                WX_STRING_REPORTER         reporter;
                IO_RELEASER<SCH_IO>        io( SCH_IO_MGR::FindPlugin( type ) );
                auto                       schematic = std::make_unique<SCHEMATIC>( &project );

                if( !io )
                    throw ERROR( CC_SCH_UNSUPPORTED, "No KiCad importer for this format" );

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
                // Importers write their symbol library beside the file they read and point the
                // project's table at ${KIPRJMOD} (EasyEDA Pro does), as when the editor imports a file
                // inside its project: read a copy in the project folder, removed afterwards.
                fs::path source = fs::absolute( fs::path( utf8( input ) ) ), inside = output.parent_path() / source.filename();
                bool     copied = false;

                if( !fs::exists( inside ) )
                    copied = fs::copy_file( source, inside );

                struct REMOVE_COPY
                {
                    const fs::path& path;
                    bool            copied;
                    ~REMOVE_COPY()
                    {
                        std::error_code ignored;

                        if( copied )
                            fs::remove( path, ignored );
                    }
                } removeCopy{ inside, copied };

                SCH_SHEET* root = io->LoadSchematicFile( wxString::FromUTF8( inside.string() ), schematic.get(), nullptr,
                                                         &properties );

                if( !root )
                    throw ERROR( CC_SCH_IO, "KiCad could not import " + utf8( input ) );

                schematic->SetTopLevelSheets( { root } );
                SCH_FOOTPRINT_FIELD_RECONCILER( cache, {}, &reporter ).Reconcile( *schematic );

                DS_DATA_MODEL::GetTheInstance().SetEmptyLayout();
                BASE_SCREEN::m_DrawingSheetFileName = "empty.kicad_wks";

                if( SCH_SHEET* top = schematic->GetTopLevelSheet() )
                    top->SetFileName( wxString::FromUTF8( output.filename().string() ) );

                schematic->RootScreen()->SetFileName( file );
                schematic->RecalculateConnections( nullptr, GLOBAL_CLEANUP, nullptr );
                schematic->RootScreen()->TestDanglingEnds();

                // SCH_EDIT_FRAME::SaveProject: each sheet file once, relative names in the folder.
                IO_RELEASER<SCH_IO>  writer( SCH_IO_MGR::FindPlugin( SCH_IO_MGR::SCH_KICAD ) );
                std::set<SCH_SCREEN*> saved;
                int                   symbols = 0;

                for( const SCH_SHEET_PATH& sheet : schematic->Hierarchy() )
                {
                    SCH_SCREEN* screen = sheet.LastScreen();
                    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
                        symbols += item != nullptr;

                    if( !saved.insert( screen ).second )
                        continue;

                    fs::path target( utf8( screen->GetFileName() ) );

                    if( target.is_relative() )
                        target = output.parent_path() / target;

                    target.replace_extension( ".kicad_sch" );
                    screen->SetFileName( wxString::FromUTF8( target.string() ) );
                    writer->SaveSchematicFile( wxString::FromUTF8( target.string() ), sheet.Last(), schematic.get() );
                }

                manager.SaveProject();
                std::ostringstream report;
                report << format << '\t' << saved.size() << '\t' << symbols << '\t'
                       << ( reporter.HasMessage() ? wxSplit( reporter.GetMessages(), '\n' ).size() : 0 );
                return owned( report.str() );
            },
            aResult, nullptr );
}
}
