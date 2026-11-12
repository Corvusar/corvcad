/////////////////////////////////////////////////////////////////////////////
// Purpose:     wxPropertyGrid choice lists without the property grid
// Author:      Jaakko Salli (wxWidgets src/propgrid/property.cpp),
//              Benjamin I. Williams (src/aui/framemanager.cpp)
// Copyright:   (c) Jaakko Salli; (C) Copyright 2005-2006, Kirix Corporation
// Licence:     wxWindows licence (wxWindows Library Licence, Version 3.1)
/////////////////////////////////////////////////////////////////////////////

// CorvCAD headless subset of wxWidgets 3.3.4. KiCad's property system stores enum choices in
// wxPGChoices and settings validate wxAuiPaneInfo, so model code needs these few functions,
// copied unchanged from the files above, but not the propgrid or AUI libraries (which have no
// iOS port). Only the parts KiCad's model calls are here; the toolbar check in IsValid() needs
// a docked window, which a headless pane never has.

#include <wx/defs.h>

// Desktop wxWidgets only: ports without propgrid or AUI (iOS) use properties/pg_choices_fallback.h
// and never see wxAuiPaneInfo.
#if wxUSE_PROPGRID
#include <wx/propgrid/property.h>


// -----------------------------------------------------------------------
// wxPGCellData, wxPGCell
// -----------------------------------------------------------------------

wxPGCellData::wxPGCellData()
    : wxObjectRefData()
    , m_hasValidText(false)
{
}

wxPGCell::wxPGCell( const wxString& text,
                    const wxBitmapBundle& bitmap,
                    const wxColour& fgCol,
                    const wxColour& bgCol )
    : wxObject()
{
    wxPGCellData* data = new wxPGCellData();
    m_refData = data;
    data->m_text = text;
    data->m_bitmapBundle = bitmap;
    data->m_fgCol = fgCol;
    data->m_bgCol = bgCol;
    data->m_hasValidText = true;
}

wxObjectRefData *wxPGCell::CloneRefData( const wxObjectRefData *data ) const
{
    wxPGCellData* c = new wxPGCellData();
    const wxPGCellData* o = (const wxPGCellData*) data;
    c->m_text = o->m_text;
    c->m_bitmapBundle = o->m_bitmapBundle;
    c->m_fgCol = o->m_fgCol;
    c->m_bgCol = o->m_bgCol;
    c->m_hasValidText = o->m_hasValidText;
    return c;
}

void wxPGCell::SetText( const wxString& text )
{
    AllocExclusive();

    GetData()->SetText(text);
}

// -----------------------------------------------------------------------
// wxPGChoiceEntry
// -----------------------------------------------------------------------

wxPGChoiceEntry::wxPGChoiceEntry()
    : wxPGCell()
    , m_value(wxPG_INVALID_VALUE)
{
}

wxPGChoiceEntry::wxPGChoiceEntry(const wxPGChoiceEntry& other)
    : wxPGCell(other)
    , m_value(other.m_value)
{
}

wxPGChoiceEntry::wxPGChoiceEntry(const wxString& label, int value)
    : wxPGCell()
    , m_value(value)
{
    SetText(label);
}

wxPGChoiceEntry& wxPGChoiceEntry::operator=(const wxPGChoiceEntry& other)
{
    if ( this != &other )
    {
        Ref(other);
    }
    m_value = other.m_value;
    return *this;
}

// -----------------------------------------------------------------------
// wxPGChoicesData
// -----------------------------------------------------------------------

wxPGChoicesData::~wxPGChoicesData()
{
    Clear();
}

void wxPGChoicesData::Clear()
{
    m_items.clear();
}

void wxPGChoicesData::CopyDataFrom(wxPGChoicesData* data)
{
    wxASSERT(m_items.empty());

    m_items = data->m_items;
}

wxPGChoiceEntry& wxPGChoicesData::Insert(int index,
    const wxPGChoiceEntry& item)
{
    std::vector<wxPGChoiceEntry>::iterator it;
    if ( index == -1 )
    {
        it = m_items.end();
        index = (int)m_items.size();
    }
    else
    {
        it = m_items.begin() + index;
    }

    m_items.insert(it, item);

    wxPGChoiceEntry& ownEntry = m_items[index];

    // Need to fix value?
    if ( ownEntry.GetValue() == wxPG_INVALID_VALUE )
        ownEntry.SetValue(index);

    return ownEntry;
}

// -----------------------------------------------------------------------
// wxPGChoices
// -----------------------------------------------------------------------

wxPGChoiceEntry& wxPGChoices::Add( const wxString& label, int value )
{
    AllocExclusive();

    wxPGChoiceEntry entry(label, value);
    return m_data->Insert( -1, entry );
}

void wxPGChoices::Clear()
{
    if ( m_data != wxPGChoicesEmptyData )
    {
        AllocExclusive();
        m_data->Clear();
    }
}

int wxPGChoices::Index( const wxString& str ) const
{
    if ( IsOk() )
    {
        for ( unsigned int i = 0; i < m_data->GetCount(); i++ )
        {
            const wxPGChoiceEntry& entry = m_data->Item(i);
            if ( entry.HasText() && entry.GetText() == str )
                return i;
        }
    }
    return -1;
}

int wxPGChoices::Index( int val ) const
{
    if ( IsOk() )
    {
        for ( unsigned int i = 0; i < m_data->GetCount(); i++ )
        {
            if ( GetValue(i) == val )
                return i;
        }
    }
    return -1;
}

void wxPGChoices::AllocExclusive()
{
    EnsureData();

    if ( m_data->GetRefCount() != 1 )
    {
        wxPGChoicesData* data = new wxPGChoicesData();
        data->CopyDataFrom(m_data);
        Free();
        m_data = data;
    }
}

void wxPGChoices::AssignData( wxPGChoicesData* data )
{
    Free();

    if ( data != wxPGChoicesEmptyData )
    {
        m_data = data;
        data->IncRef();
    }
}

void wxPGChoices::Init()
{
    m_data = wxPGChoicesEmptyData;
}

void wxPGChoices::Free()
{
    if ( m_data != wxPGChoicesEmptyData )
    {
        m_data->DecRef();
        m_data = wxPGChoicesEmptyData;
    }
}

#endif // wxUSE_PROPGRID

#if wxUSE_AUI
#include <wx/aui/framemanager.h>


bool wxAuiPaneInfo::IsValid() const
{
    if ( dock_direction == wxAUI_DOCK_CENTRE &&
        !(dock_layer == 0 && dock_row == 0 && dock_pos == 0) )
    {
        wxFAIL_MSG("Center pane must have dock layer, row and pos set to 0");

        return false;
    }

    return true;
}

#endif // wxUSE_AUI
