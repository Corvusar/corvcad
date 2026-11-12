/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2016 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * @author Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
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

// CorvCAD headless replacement for pcbnew/board_commit.cpp (KiCad 10.0.6).
//
// The upstream commit drives the board editor: view, selection tool, undo list, teardrop and
// zone-fill events and ratsnest redraw through the TOOL_MANAGER. Without a tool manager this
// version only applies the staged changes to the board, notifies it (OnItemsCompositeUpdate)
// and rebuilds connectivity, as upstream does for the model. The board comes from the staged
// items (no headless caller has a tool manager). There is no undo list, so
// replaced copies and removed items are freed once the board no longer refers to them.
// Callers that refill zones or regenerate teardrops do so themselves (kicad_board.cpp).

#include <board.h>
#include <board_commit.h>
#include <component_classes/component_class_manager.h>
#include <footprint.h>
#include <macros.h>
#include <pcb_group.h>


BOARD_COMMIT::BOARD_COMMIT( TOOL_BASE* aTool ) :
        m_toolMgr( nullptr ),
        m_isBoardEditor( false ),
        m_isFootprintEditor( false )
{
}


BOARD_COMMIT::BOARD_COMMIT( EDA_DRAW_FRAME* aFrame ) :
        m_toolMgr( nullptr ),
        m_isBoardEditor( false ),
        m_isFootprintEditor( false )
{
}


BOARD_COMMIT::BOARD_COMMIT( TOOL_MANAGER* aMgr ) :
        m_toolMgr( aMgr ),
        m_isBoardEditor( false ),
        m_isFootprintEditor( false )
{
}


BOARD_COMMIT::BOARD_COMMIT( TOOL_MANAGER* aMgr, bool aIsBoardEditor, bool aIsFootprintEditor ) :
        m_toolMgr( aMgr ),
        m_isBoardEditor( aIsBoardEditor ),
        m_isFootprintEditor( aIsFootprintEditor )
{
}


BOARD* BOARD_COMMIT::GetBoard() const
{
    for( const COMMIT_LINE& entry : m_entries )
    {
        if( entry.m_item && entry.m_item->IsBOARD_ITEM() )
        {
            if( BOARD* board = static_cast<BOARD_ITEM*>( entry.m_item )->GetBoard() )
                return board;
        }
    }

    return nullptr;
}


COMMIT& BOARD_COMMIT::Stage( EDA_ITEM* aItem, CHANGE_TYPE aChangeType, BASE_SCREEN* aScreen, RECURSE_MODE aRecurse )
{
    if( aRecurse == RECURSE_MODE::RECURSE )
    {
        if( PCB_GROUP* group = dynamic_cast<PCB_GROUP*>( aItem ) )
        {
            for( EDA_ITEM* member : group->GetItems() )
                Stage( member, aChangeType, aScreen, aRecurse );
        }
    }

    return COMMIT::Stage( aItem, aChangeType );
}


COMMIT& BOARD_COMMIT::Stage( std::vector<EDA_ITEM*>& container, CHANGE_TYPE aChangeType, BASE_SCREEN* aScreen )
{
    return COMMIT::Stage( container, aChangeType, aScreen );
}


COMMIT& BOARD_COMMIT::Stage( const PICKED_ITEMS_LIST& aItems, UNDO_REDO aModFlag, BASE_SCREEN* aScreen )
{
    return COMMIT::Stage( aItems, aModFlag, aScreen );
}


void BOARD_COMMIT::Push( const wxString& aMessage, int aCommitFlags )
{
    BOARD* board = GetBoard();

    if( !board )
    {
        clear();
        return;
    }

    board->IncrementTimeStamp(); // clear caches

    std::vector<BOARD_ITEM*> bulkAddedItems;
    std::vector<BOARD_ITEM*> bulkRemovedItems;
    std::vector<BOARD_ITEM*> itemsChanged;
    std::vector<BOARD_ITEM*> itemsToDelete;

    for( COMMIT_LINE& entry : m_entries )
    {
        if( !entry.m_item || !entry.m_item->IsBOARD_ITEM() )
            continue;

        BOARD_ITEM* boardItem = static_cast<BOARD_ITEM*>( entry.m_item );
        int         changeType = entry.m_type & CHT_TYPE;
        int         changeFlags = entry.m_type & CHT_FLAGS;

        switch( changeType )
        {
        case CHT_ADD:
            if( !( changeFlags & CHT_DONE ) )
                board->Add( boardItem, ADD_MODE::BULK_INSERT );

            bulkAddedItems.push_back( boardItem );
            break;

        case CHT_REMOVE:
            if( !( changeFlags & CHT_DONE ) )
                board->Remove( boardItem, REMOVE_MODE::BULK );

            bulkRemovedItems.push_back( boardItem );

            // A hidden field still belongs to its footprint (as upstream keeps it).
            if( boardItem->Type() != PCB_FIELD_T )
                itemsToDelete.push_back( boardItem );

            break;

        case CHT_MODIFY:
            itemsChanged.push_back( boardItem );
            break;

        default:
            UNIMPLEMENTED_FOR( boardItem->GetClass() );
            break;
        }

        delete entry.m_copy;
        entry.m_copy = nullptr;
        boardItem->ClearEditFlags();
    }

    board->GetComponentClassManager().InvalidateComponentClasses();

    if( !bulkAddedItems.empty() || !bulkRemovedItems.empty() || !itemsChanged.empty() )
        board->OnItemsCompositeUpdate( bulkAddedItems, bulkRemovedItems, itemsChanged );

    if( !( aCommitFlags & SKIP_CONNECTIVITY ) )
        board->BuildConnectivity();

    for( BOARD_ITEM* item : itemsToDelete )
        delete item;

    clear();
}


EDA_ITEM* BOARD_COMMIT::undoLevelItem( EDA_ITEM* aItem ) const
{
    EDA_ITEM* parent = aItem->GetParent();

    if( parent && parent->Type() == PCB_TABLE_T )
        return parent;

    return aItem;
}


EDA_ITEM* BOARD_COMMIT::makeImage( EDA_ITEM* aItem ) const
{
    return MakeImage( aItem );
}


EDA_ITEM* BOARD_COMMIT::MakeImage( EDA_ITEM* aItem )
{
    EDA_ITEM* clone = aItem->Clone();
    clone->SetFlags( UR_TRANSIENT );

    return clone;
}


// Upstream's Revert without the view, the selection tool and the footprint editor.
void BOARD_COMMIT::Revert()
{
    BOARD* board = GetBoard();

    if( !board )
    {
        clear();
        return;
    }

    board->IncrementTimeStamp(); // clear caches

    std::vector<BOARD_ITEM*> bulkAddedItems;
    std::vector<BOARD_ITEM*> bulkRemovedItems;
    std::vector<BOARD_ITEM*> itemsChanged;
    std::vector<BOARD_ITEM*> itemsToDelete;

    for( COMMIT_LINE& entry : m_entries )
    {
        if( !entry.m_item || !entry.m_item->IsBOARD_ITEM() )
            continue;

        BOARD_ITEM* boardItem = static_cast<BOARD_ITEM*>( entry.m_item );
        int         changeType = entry.m_type & CHT_TYPE;
        int         changeFlags = entry.m_type & CHT_FLAGS;

        switch( changeType )
        {
        case CHT_ADD:
            if( changeFlags & CHT_DONE )
            {
                board->Remove( boardItem, REMOVE_MODE::BULK );
                bulkRemovedItems.push_back( boardItem );
            }

            if( PCB_GROUP* group = dynamic_cast<PCB_GROUP*>( boardItem ) )
                group->RemoveAll();

            itemsToDelete.push_back( boardItem );
            entry.m_item = nullptr;
            continue;

        case CHT_REMOVE:
            if( !( changeFlags & CHT_DONE ) )
                break;

            board->Add( boardItem, ADD_MODE::INSERT );
            bulkAddedItems.push_back( boardItem );
            break;

        case CHT_MODIFY:
        {
            wxASSERT( entry.m_copy && entry.m_copy->IsBOARD_ITEM() );
            boardItem->SwapItemData( static_cast<BOARD_ITEM*>( entry.m_copy ) );
            itemsChanged.push_back( boardItem );
            break;
        }

        default:
            UNIMPLEMENTED_FOR( boardItem->GetClass() );
            break;
        }

        delete entry.m_copy;
        entry.m_copy = nullptr;
        boardItem->ClearEditFlags();
    }

    board->GetComponentClassManager().InvalidateComponentClasses();

    if( !bulkAddedItems.empty() || !bulkRemovedItems.empty() || !itemsChanged.empty() )
        board->OnItemsCompositeUpdate( bulkAddedItems, bulkRemovedItems, itemsChanged );

    board->BuildConnectivity();

    for( BOARD_ITEM* item : itemsToDelete )
        delete item;

    clear();
}
