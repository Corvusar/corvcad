/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
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
// CorvCAD iOS stand-in for KiCad 10.0.6 include/kidialog.h. KIDIALOG derives from
// wxRichMessageDialog, which wxWidgets' iOS port does not have. The board slice reaches it
// only through the zone filler's refill prompts (ZONE_FILLER::Fill with aCheck or a parent
// window), which a headless caller never asks for, and the router's warning about a malformed
// keepout zone (PNS_KICAD_IFACE_BASE::syncZone). KIDIALOG below is upstream's class,
// unchanged; KIDIALOG_BASE is a window-less message box with the members kidialog.cpp uses.
// ShowModal logs the message (as headless/confirm.cpp does) and answers Cancel: nobody
// confirmed.

#pragma once

#include <kicommon.h>

#include <wx/defs.h>
#include <wx/intl.h>
#include <wx/log.h>
#include <wx/string.h>
#include <wx/toplevel.h> // wxSTAY_ON_TOP, which upstream gets through wx/richmsgdlg.h

class wxWindow;

class KIDIALOG_BASE
{
public:
    // wxMessageDialogBase::ButtonLabel: a label given as text or as a stock id.
    struct ButtonLabel
    {
        ButtonLabel( const wxString& ) {}
        ButtonLabel( const char* ) {}
        ButtonLabel( int ) {}
    };

    KIDIALOG_BASE( wxWindow*, const wxString& aMessage, const wxString& aCaption, long ) :
            m_message( aMessage ),
            m_caption( aCaption )
    {
    }
    virtual ~KIDIALOG_BASE() = default;

    virtual bool SetOKCancelLabels( const ButtonLabel&, const ButtonLabel& ) { return true; }
    void ShowCheckBox( const wxString&, bool = false ) {}
    void ShowDetailedText( const wxString& aText ) { m_message << wxS( "\n" ) << aText; }
    bool IsCheckBoxChecked() const { return false; }
    virtual bool Show( bool = true ) { return false; }

    virtual int ShowModal()
    {
        wxLogWarning( wxS( "%s: %s" ), m_caption, m_message );
        return wxID_CANCEL;
    }

private:
    wxString m_message;
    wxString m_caption;
};


/**
 * Helper class to create more flexible dialogs, including 'do not show again' checkbox handling.
 */
class KICOMMON_API KIDIALOG : public KIDIALOG_BASE
{
public:
    static void ClearDoNotShowAgainDialogs();

    ///< Dialog type. Selects appropriate icon and default dialog title
    enum KD_TYPE { KD_NONE, KD_INFO, KD_QUESTION, KD_WARNING, KD_ERROR };

    KIDIALOG( wxWindow* aParent, const wxString& aMessage, const wxString& aCaption, long aStyle = wxOK );
    KIDIALOG( wxWindow* aParent, const wxString& aMessage, KD_TYPE aType, const wxString& aCaption = "" );

    bool SetOKCancelLabels( const ButtonLabel& ok, const ButtonLabel& cancel ) override
    {
        m_cancelMeansCancel = false;
        return KIDIALOG_BASE::SetOKCancelLabels( ok, cancel );
    }

    /// Shows the 'do not show again' checkbox.
    void DoNotShowCheckbox( wxString file, int line );

    /// Checks the 'do not show again' setting for the dialog.
    bool DoNotShowAgain() const;

    bool Show( bool aShow = true ) override;
    int ShowModal() override;

protected:
    // Helper functions for wxRichMessageDialog constructor
    static wxString getCaption( KD_TYPE aType, const wxString& aCaption );
    static long getStyle( KD_TYPE aType );

protected:
    unsigned long m_hash;               // Unique id
    bool          m_cancelMeansCancel;  // If the Cancel button is renamed then it should be saved by the
                                        // DoNotShowAgain checkbox.  If it's really a cancel then it should not.
};
