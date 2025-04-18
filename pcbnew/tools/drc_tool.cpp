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

#include <pcb_edit_frame.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_tool_base.h>
#include <tools/zone_filler_tool.h>
#include <tools/pcb_selection_tool.h>
#include <tools/drc_tool.h>
#include <kiface_base.h>
#include <dialog_drc.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <progress_reporter.h>
#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <netlist_reader/pcb_netlist.h>
#include <macros.h>

DRC_TOOL::DRC_TOOL() :
        PCB_TOOL_BASE( "pcbnew.DRCTool" ),
        m_editFrame( nullptr ),
        m_pcb( nullptr ),
        m_drcDialog( nullptr ),
        m_drcRunning( false )
{
}


DRC_TOOL::~DRC_TOOL()
{
}


void DRC_TOOL::Reset( RESET_REASON aReason )
{
    m_editFrame = getEditFrame<PCB_EDIT_FRAME>();

    if( m_pcb != m_editFrame->GetBoard() )
    {
        if( m_drcDialog )
            DestroyDRCDialog();

        m_pcb = m_editFrame->GetBoard();
        m_drcEngine = m_pcb->GetDesignSettings().m_DRCEngine;
    }
}


void DRC_TOOL::ShowDRCDialog( wxWindow* aParent )
{
    bool show_dlg_modal = true;

    // the dialog needs a parent frame. if it is not specified, this is the PCB editor frame
    // specified in DRC_TOOL class.
    if( !aParent )
    {
        // if any parent is specified, the dialog is modal.
        // if this is the default PCB editor frame, it is not modal
        show_dlg_modal = false;
        aParent = m_editFrame;
    }

    Activate();
    m_toolMgr->RunAction( ACTIONS::selectionClear );

    if( !m_drcDialog )
    {
        m_drcDialog = new DIALOG_DRC( m_editFrame, aParent );
        updatePointers( false );

        if( show_dlg_modal )
            m_drcDialog->ShowModal();
        else
            m_drcDialog->Show( true );
    }
    else // The dialog is just not visible (because the user has double clicked on an error item)
    {
        updatePointers( false );
        m_drcDialog->Show( true );
    }
}


int DRC_TOOL::ShowDRCDialog( const TOOL_EVENT& aEvent )
{
    ShowDRCDialog( nullptr );
    return 0;
}


bool DRC_TOOL::IsDRCDialogShown()
{
    if( m_drcDialog )
        return m_drcDialog->IsShownOnScreen();

    return false;
}


void DRC_TOOL::DestroyDRCDialog()
{
    if( m_drcDialog )
    {
        m_drcDialog->Destroy();
        m_drcDialog = nullptr;
    }
}


void DRC_TOOL::RunTests( PROGRESS_REPORTER* aProgressReporter, bool aRefillZones,
                         bool aReportAllTrackErrors, bool aTestFootprints )
{
    // One at a time, please.
    // Note that the main GUI entry points to get here are blocked, so this is really an
    // insurance policy and as such we make no attempts to queue up the DRC run or anything.
    if( m_drcRunning )
        return;

    ZONE_FILLER_TOOL* zoneFiller = m_toolMgr->GetTool<ZONE_FILLER_TOOL>();
    BOARD_COMMIT      commit( m_editFrame );
    NETLIST           netlist;
    bool              netlistFetched = false;
    wxWindowDisabler  disabler( /* disable everything except: */ m_drcDialog );

    m_drcRunning = true;

    if( aRefillZones )
    {
        aProgressReporter->AdvancePhase( _( "Refilling all zones..." ) );

        zoneFiller->FillAllZones( m_drcDialog, aProgressReporter );
    }

    m_drcEngine->SetDrawingSheet( m_editFrame->GetCanvas()->GetDrawingSheet() );

    if( aTestFootprints && !Kiface().IsSingle() )
    {
        if( m_editFrame->FetchNetlistFromSchematic( netlist, _( "Schematic parity tests require a "
                                                                "fully annotated schematic." ) ) )
        {
            netlistFetched = true;
        }

        if( m_drcDialog )
            m_drcDialog->Raise();

        m_drcEngine->SetSchematicNetlist( &netlist );
    }

    m_drcEngine->SetProgressReporter( aProgressReporter );

    m_drcEngine->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, VECTOR2I aPos, int aLayer,
                 DRC_CUSTOM_MARKER_HANDLER* aCustomHandler )
            {
                PCB_MARKER* marker = new PCB_MARKER( aItem, aPos, aLayer );

                if( aCustomHandler )
                    ( *aCustomHandler )( marker );

                commit.Add( marker );
            } );

    m_drcEngine->RunTests( m_editFrame->GetUserUnits(), aReportAllTrackErrors, aTestFootprints,
                           &commit );

    m_drcEngine->SetProgressReporter( nullptr );
    m_drcEngine->ClearViolationHandler();

    if( m_drcDialog )
    {
        m_drcDialog->SetDrcRun();

        if( aTestFootprints && netlistFetched )
            m_drcDialog->SetFootprintTestsRun();
    }

    commit.Push( _( "DRC" ), SKIP_UNDO | SKIP_SET_DIRTY );

    m_drcRunning = false;

    m_editFrame->ShowSolderMask();

    // update the m_drcDialog listboxes
    updatePointers( aProgressReporter->IsCancelled() );
}


void DRC_TOOL::updatePointers( bool aDRCWasCancelled )
{
    // update my pointers, m_editFrame is the only unchangeable one
    m_pcb = m_editFrame->GetBoard();

    m_editFrame->ResolveDRCExclusions( aDRCWasCancelled );

    if( m_drcDialog )
        m_drcDialog->UpdateData();
}


int DRC_TOOL::PrevMarker( const TOOL_EVENT& aEvent )
{
    if( m_drcDialog )
    {
        m_drcDialog->Show( true );
        m_drcDialog->Raise();
        m_drcDialog->PrevMarker();
    }
    else
    {
        ShowDRCDialog( nullptr );
    }

    return 0;
}


int DRC_TOOL::NextMarker( const TOOL_EVENT& aEvent )
{
    if( m_drcDialog )
    {
        m_drcDialog->Show( true );
        m_drcDialog->Raise();
        m_drcDialog->NextMarker();
    }
    else
    {
        ShowDRCDialog( nullptr );
    }

    return 0;
}


int DRC_TOOL::CrossProbe( const TOOL_EVENT& aEvent )
{
    if( m_drcDialog && m_drcDialog->IsShownOnScreen() )
    {
        PCB_SELECTION_TOOL* selectionTool = m_toolMgr->GetTool<PCB_SELECTION_TOOL>();
        PCB_SELECTION&      selection = selectionTool->GetSelection();

        if( selection.GetSize() == 1 && selection.Front()->Type() == PCB_MARKER_T )
            m_drcDialog->SelectMarker( static_cast<PCB_MARKER*>( selection.Front() ) );
    }

    return 0;
}


void DRC_TOOL::CrossProbe( const PCB_MARKER* aMarker )
{
    if( !IsDRCDialogShown() )
        ShowDRCDialog( nullptr );

    m_drcDialog->SelectMarker( aMarker );
}


int DRC_TOOL::ExcludeMarker( const TOOL_EVENT& aEvent )
{
    if( m_drcDialog )
        m_drcDialog->ExcludeMarker();

    return 0;
}


void DRC_TOOL::setTransitions()
{
    Go( &DRC_TOOL::ShowDRCDialog,              PCB_ACTIONS::runDRC.MakeEvent() );
    Go( &DRC_TOOL::PrevMarker,                 ACTIONS::prevMarker.MakeEvent() );
    Go( &DRC_TOOL::NextMarker,                 ACTIONS::nextMarker.MakeEvent() );
    Go( &DRC_TOOL::ExcludeMarker,              ACTIONS::excludeMarker.MakeEvent() );
    Go( &DRC_TOOL::CrossProbe,                 EVENTS::PointSelectedEvent );
    Go( &DRC_TOOL::CrossProbe,                 EVENTS::SelectedEvent );
}
