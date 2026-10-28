/**
 * @file lltoolbarview.h
 * @author Merov Linden
 * @brief User customizable toolbar class
 *
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2011, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLTOOLBARVIEW_H
#define LL_LLTOOLBARVIEW_H

#include "lluictrl.h"
#include "lltoolbar.h"
#include "llcommandmanager.h"
#include "llinventory.h"

class LLUICtrlFactory;

// Parent of all LLToolBar

class LLToolBarView : public LLUICtrl
{
public:
    // Xui structure of the toolbar panel
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params> {};

    // Note: valid children for LLToolBarView are stored in this registry
    typedef LLDefaultChildRegistry child_registry_t;

    // Xml structure of the toolbars.xml setting
    // Those live in a toolbars.xml found in the selected skin folder and in
    // the user folder for the user specific (saved) settings
    struct Toolbar : public LLInitParam::Block<Toolbar>
    {
        Mandatory<LLToolBarEnums::ButtonType>   button_display_mode;
        Multiple<LLCommandId::Params>   commands;
        Optional<LLToolBarEnums::Alignment>     button_alignment;       // <FS:Zi> Added alignment parameter
        Optional<LLToolBarEnums::LayoutStyle>   button_layout_style;    // <FS:Zi> Added layout style parameter

        Toolbar();
    };
    struct ToolbarSet : public LLInitParam::Block<ToolbarSet>
    {
        Optional<Toolbar>   left_toolbar,
                            right_toolbar,
                            bottom_toolbar;

        ToolbarSet();
    };

    // Derived methods
    virtual ~LLToolBarView();
    bool postBuild() override;
    void draw() override;

    // Toolbar view interface with the rest of the world
    // Checks if the commandId is being used somewhere in one of the toolbars, returns LLToolBarEnums::EToolBarLocation
    S32 hasCommand(const LLCommandId& commandId) const;
    S32 addCommand(const LLCommandId& commandId, LLToolBarEnums::EToolBarLocation toolbar, int rank = LLToolBar::RANK_NONE);
    S32 removeCommand(const LLCommandId& commandId, int& rank); // Sets the rank the removed command was at, RANK_NONE if not found
    // <WolfViewer 2026-10-20> Paul: "DO NOT CHANGE PEOPLES BUTTONS they should be saved". A Wolf-only button is taken off
    // its toolbar off Wolf Territories (a toolbar lays out hidden buttons too) WITHOUT leaving the person's layout: where
    // it was is remembered by its neighbours, it goes back there however many others come and go, and saveToolbars
    // writes it in its place. Idempotent - the grid gates call it every frame / second.
    void setCommandGated(const LLCommandId& commandId, bool gated);
    // </WolfViewer>
    S32 enableCommand(const LLCommandId& commandId, bool enabled);
    S32 stopCommandInProgress(const LLCommandId& commandId);
    S32 flashCommand(const LLCommandId& commandId, bool flash, bool force_flashing = false);

    // Loads the toolbars from the existing user or default settings
    bool loadToolbars(bool force_default = false);  // return false if load fails

    // Clears all buttons off the toolbars
    bool clearToolbars();

    void setToolBarsVisible(bool visible);

    static bool loadDefaultToolbars();
    static bool clearAllToolbars();

    static void startDragTool(S32 x, S32 y, LLToolBarButton* toolbarButton);
    static bool handleDragTool(S32 x, S32 y, const LLUUID& uuid, LLAssetType::EType type);
    static bool handleDropTool(void* cargo_data, EDragAndDropType cargo_type, S32 x, S32 y, LLToolBar* toolbar);
    static void resetDragTool(LLToolBarButton* toolbarButton);
    LLInventoryObject* getDragItem();
    LLView* getBottomToolbar() { return mBottomToolbarPanel; }
    LLToolBar* getToolbar(LLToolBarEnums::EToolBarLocation toolbar) { return mToolbars[toolbar]; }
    bool isModified() const;

    // <FS:Ansariel> Getters for member variables needed for console chat bottom offset
    LLView* getBottomChatStack() const { return mBottomChatStack; }
    // </FS:Ansariel>

    // <WolfViewer 2026-09-08> Size the strip left of the bottom toolbar to whatever it
    // currently has to show — the chat bar, the Stand / Stop Flying buttons, or nothing —
    // so the two-row toolbar is centred on the SCREEN and not on the space left over.
    // Call after anything that changes either of those. See lltoolbarview.cpp.
    static void refreshChatStripWidth();
    // <WolfViewer 2026-09-10> the arrow at the bottom bar's right end (panel_toolbar_view.xml)
    void toggleBottomToolbar();
    void applyBottomToolbarHidden(bool hidden);
    // </WolfViewer>
    // <WolfViewer 2026-10-20> Single bar (WolfViewerToolbarSingleBar): the height bottom_toolbar_panel stands
    // at while the bar shows - the skin's own height normally, just what one row needs in single bar.
    S32 bottomShownDim() const;
    // </WolfViewer>

    // <FS:Ansariel> Added to determine if toolbar gets hidden when empty
    void setHideBottomOnEmpty(bool hideBottomOnEmpty) { mHideBottomOnEmpty = hideBottomOnEmpty; }

protected:
    friend class LLUICtrlFactory;
    LLToolBarView(const Params&);

    void initFromParams(const Params&);

private:
    // <WolfViewer 2026-10-20> a gated button: its toolbar, the buttons before and after it there when it was taken off
    struct WolfGated { S32 toolbar = 0; int rank = -1; std::vector<LLUUID> before, after; };
    std::map<LLUUID, std::pair<LLCommandId, WolfGated>> mWolfGated;
    static size_t wolfGatedIndex(const std::vector<LLCommandId>& list, const WolfGated& g);
    void wolfWithGated(S32 toolbar, std::vector<LLCommandId>& list) const;   // the list with its gated buttons back in place
    // </WolfViewer>
    void    saveToolbars() const;
    bool    addCommandInternal(const LLCommandId& commandId, LLToolBar* toolbar);
    void    addToToolset(command_id_list_t& command_list, Toolbar& toolbar) const;

    static void onToolBarButtonAdded(LLView* button);
    static void onToolBarButtonRemoved(LLView* button);

    // Pointers to the toolbars handled by the toolbar view
    LLToolBar*  mToolbars[LLToolBarEnums::TOOLBAR_COUNT];
    bool        mToolbarsLoaded;

    bool                mDragStarted;
    LLToolBarButton*    mDragToolbarButton;
    LLInventoryObject*  mDragItem;
    bool                mShowToolbars;
    LLView*             mBottomToolbarPanel;
    LLButton*           mWolfToolbarToggle = nullptr;   // <WolfViewer 2026-09-10>
    S32                 mBottomToolbarShownDim = 90;    // <WolfViewer 2026-10-06> the skin's own bottom_toolbar_panel height
    bool                mWolfSingleBarApplied = false;  // <WolfViewer 2026-10-20> single bar state last given to the bar
    LLButton*           mWolfPlaneToggle = nullptr;     // <WolfViewer 2026-10-06> Flight Mode deck
    LLButton*           mWolfBoatToggle = nullptr;      // <WolfViewer 2026-10-06> Sailing Mode deck
    LLButton*           mWolfGameToggle = nullptr;      // <WolfViewer 2026-10-09> Game Mode
    LLButton*           mWolfCarToggle = nullptr;       // <WolfViewer 2026-10-09> Car Dashboard
    LLButton*           mWolfSpeedboatToggle = nullptr; // <WolfViewer 2026-10-11> Speedboat Helm (w65)
    LLButton*           mWolfSpaceshipToggle = nullptr; // <WolfViewer 2026-10-11> Spaceship HUD (w65)
    LLButton*           mWolfSubmarineToggle = nullptr; // <WolfViewer 2026-10-11> Submarine Helm (w65)
    LLButton*           mWolfShipToggle = nullptr;      // <WolfViewer 2026-10-28> Captain's Helm (sailing ships)
    bool                mWolfPanelUp = false;           // <WolfViewer 2026-10-09> a bottom panel (deck, dashboard, game HUD) shows
    bool                mWolfBarWasHidden = false;      // <WolfViewer 2026-10-09> the bar's own state before it did

    // <FS:Ansariel> Member variables needed for console chat bottom offset
    LLView*             mBottomChatStack;
    // </FS:Ansariel>

    // <FS:Ansariel> Added to determine if toolbar gets hidden when empty
    bool                mHideBottomOnEmpty;
};

extern LLToolBarView* gToolBarView;

#endif  // LL_LLTOOLBARVIEW_H
