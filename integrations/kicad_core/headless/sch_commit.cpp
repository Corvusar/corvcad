/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
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

// CorvCAD headless replacement for eeschema/sch_commit.cpp (KiCad 10.0.6).
//
// The upstream commit drives the editor: view, selection tool, undo list, symbol editor and
// connectivity refresh through SCH_EDIT_FRAME. Without a frame this version only applies the
// staged changes to their screens and notifies the schematic, as upstream does when
// GetToolHolder() is null. Callers recalculate connectivity themselves.

#include <sch_commit.h>

#include <sch_field.h>
#include <sch_group.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <connection_graph.h>


SCH_COMMIT::SCH_COMMIT( TOOL_MANAGER* aToolMgr ) :
        COMMIT(),
        m_toolMgr( aToolMgr ),
        m_isLibEditor( false )
{
}


SCH_COMMIT::~SCH_COMMIT()
{
}


COMMIT& SCH_COMMIT::Stage( EDA_ITEM* aItem, CHANGE_TYPE aChangeType, BASE_SCREEN* aScreen,
                           RECURSE_MODE aRecurse )
{
    wxCHECK( aItem, *this );

    if( aRecurse == RECURSE_MODE::RECURSE )
    {
        if( SCH_GROUP* group = dynamic_cast<SCH_GROUP*>( aItem ) )
        {
            for( EDA_ITEM* member : group->GetItems() )
                Stage( member, aChangeType, aScreen, aRecurse );
        }
    }

    COMMIT::Stage( aItem, aChangeType, aScreen );
    return *this;
}


COMMIT& SCH_COMMIT::Stage( std::vector<EDA_ITEM*>& container, CHANGE_TYPE aChangeType,
                           BASE_SCREEN* aScreen )
{
    for( EDA_ITEM* item : container )
        Stage( item, aChangeType, aScreen );

    return *this;
}


void SCH_COMMIT::Push( const wxString& aMessage, int aCommitFlags )
{
    if( Empty() )
        return;

    SCHEMATIC*             schematic = nullptr;
    std::vector<SCH_ITEM*> bulkAddedItems;
    std::vector<SCH_ITEM*> bulkRemovedItems;
    std::vector<SCH_ITEM*> itemsChanged;
    bool                   refreshHierarchy = false;

    for( COMMIT_LINE& entry : m_entries )
    {
        int         changeType = entry.m_type & CHT_TYPE;
        int         changeFlags = entry.m_type & CHT_FLAGS;
        SCH_ITEM*   schItem = dynamic_cast<SCH_ITEM*>( entry.m_item );
        SCH_SCREEN* screen = dynamic_cast<SCH_SCREEN*>( entry.m_screen );

        wxCHECK2( schItem, continue );
        wxCHECK2( screen, continue );

        if( !schematic )
            schematic = schItem->Schematic();

        switch( changeType )
        {
        case CHT_ADD:
            if( !( changeFlags & CHT_DONE ) && !screen->CheckIfOnDrawList( schItem ) )
                screen->Append( schItem );

            screen->Update( schItem );
            bulkAddedItems.push_back( schItem );
            refreshHierarchy |= schItem->Type() == SCH_SHEET_T;
            break;

        case CHT_REMOVE:
            if( schItem->Type() == SCH_FIELD_T )
            {
                static_cast<SCH_FIELD*>( schItem )->SetVisible( false );
                break;
            }

            if( EDA_GROUP* group = schItem->GetParentGroup() )
                group->RemoveItem( schItem );

            if( !( changeFlags & CHT_DONE ) )
                screen->Remove( schItem );

            // ponytail: upstream hands removed items to the editor's undo list, which frees
            // them; there is none here, so they stay allocated until an undo design exists.
            bulkRemovedItems.push_back( schItem );
            refreshHierarchy |= schItem->Type() == SCH_SHEET_T;
            break;

        case CHT_MODIFY:
            if( schItem->Type() == SCH_SHEET_T )
            {
                const SCH_SHEET* original = static_cast<const SCH_SHEET*>( entry.m_copy );
                refreshHierarchy |= original
                                    && original->HasPageNumberChanges(
                                            *static_cast<const SCH_SHEET*>( schItem ) );
            }

            screen->Update( schItem );
            itemsChanged.push_back( schItem );
            break;

        default:
            wxASSERT( false );
            break;
        }

        delete entry.m_copy;
        entry.m_copy = nullptr;

        // Same flag reset as upstream: keep selection and move/rotate flags only.
        const int selected_mask = ( SELECTED | STARTPOINT | ENDPOINT );
        schItem->ClearFlags( EDA_ITEM_ALL_FLAGS - selected_mask );

        if( schItem->Type() == SCH_SHEET_T || schItem->Type() == SCH_SYMBOL_T )
        {
            schItem->RunOnChildren(
                    [&]( SCH_ITEM* child )
                    {
                        child->ClearFlags( EDA_ITEM_ALL_FLAGS - selected_mask );
                    },
                    RECURSE_MODE::NO_RECURSE );
        }
    }

    if( schematic )
    {
        if( !bulkAddedItems.empty() )
            schematic->OnItemsAdded( bulkAddedItems );

        if( !bulkRemovedItems.empty() )
            schematic->OnItemsRemoved( bulkRemovedItems );

        if( !itemsChanged.empty() )
            schematic->OnItemsChanged( itemsChanged );

        if( refreshHierarchy )
            schematic->RefreshHierarchy();
    }

    clear();
}


EDA_ITEM* SCH_COMMIT::undoLevelItem( EDA_ITEM* aItem ) const
{
    EDA_ITEM* parent = aItem->GetParent();

    if( parent && parent->IsType( { SCH_SYMBOL_T, SCH_TABLE_T, SCH_SHEET_T, SCH_LABEL_LOCATE_ANY_T } ) )
        return parent;

    return aItem;
}


EDA_ITEM* SCH_COMMIT::makeImage( EDA_ITEM* aItem ) const
{
    return aItem->Clone();
}


void SCH_COMMIT::Revert()
{
    SCH_SHEET_LIST sheets;

    if( m_entries.empty() )
        return;

    SCHEMATIC*             schematic = nullptr;
    std::vector<SCH_ITEM*> bulkAddedItems;
    std::vector<SCH_ITEM*> bulkRemovedItems;

    for( COMMIT_LINE& ent : m_entries )
    {
        int         changeType = ent.m_type & CHT_TYPE;
        int         changeFlags = ent.m_type & CHT_FLAGS;
        SCH_ITEM*   item = dynamic_cast<SCH_ITEM*>( ent.m_item );
        SCH_ITEM*   copy = dynamic_cast<SCH_ITEM*>( ent.m_copy );
        SCH_SCREEN* screen = dynamic_cast<SCH_SCREEN*>( ent.m_screen );

        wxCHECK2( item && screen, continue );

        if( !schematic )
            schematic = item->Schematic();

        switch( changeType )
        {
        case CHT_ADD:
            if( !( changeFlags & CHT_DONE ) )
                break;

            screen->Remove( item );
            bulkRemovedItems.push_back( item );
            break;

        case CHT_REMOVE:
            item->SetConnectivityDirty();

            if( !( changeFlags & CHT_DONE ) )
                break;

            screen->Append( item );
            bulkAddedItems.push_back( item );
            break;

        case CHT_MODIFY:
        {
            wxCHECK2( copy, break );

            item->SwapItemData( copy );

            // Items with instance data: restore the reference for this screen's sheet.
            if( item->GetParent() && item->GetParent()->Type() == SCH_SYMBOL_T && item->Type() == SCH_FIELD_T )
            {
                SCH_FIELD*  field = static_cast<SCH_FIELD*>( item );
                SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item->GetParent() );

                if( field->GetId() == FIELD_T::REFERENCE )
                {
                    if( sheets.empty() )
                        sheets = schematic->Hierarchy();

                    SCH_SHEET_PATH sheet = sheets.FindSheetForScreen( screen );
                    symbol->SetRef( &sheet, field->GetText() );
                }
            }

            screen->Update( item );

            // Keep stale pins of the swapped-out copy out of the connection graph.
            if( item->Type() == SCH_SYMBOL_T )
            {
                SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
                symbol->UpdatePins();

                CONNECTION_GRAPH* graph = schematic->ConnectionGraph();
                SCH_SYMBOL*       symbolCopy = static_cast<SCH_SYMBOL*>( copy );
                graph->RemoveItem( symbolCopy );

                for( SCH_PIN* pin : symbolCopy->GetPins() )
                    graph->RemoveItem( pin );
            }

            item->SetConnectivityDirty();
            delete copy;
            break;
        }

        default:
            wxASSERT( false );
            break;
        }
    }

    if( schematic )
    {
        if( !bulkAddedItems.empty() )
            schematic->OnItemsAdded( bulkAddedItems );

        if( !bulkRemovedItems.empty() )
            schematic->OnItemsRemoved( bulkRemovedItems );
    }

    clear();
}
