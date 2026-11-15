/*
 * SPDX-FileCopyrightText: 2014 Hugo Pereira Da Costa <hugo.pereira@free.fr>
 * SPDX-FileCopyrightText: 2020 Noah Davis <noahadvs@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QtGlobal>

namespace Breeze
{
struct PenWidth {
    static constexpr qreal Symbol = 1.001;

    static constexpr qreal Frame = 1.001;

    static constexpr qreal Shadow = 1.001;

    static constexpr int NoPen = 0;
};

struct Metrics {
    static constexpr int ArrowSize = 10;
    static constexpr int SmallArrowSize = 5;

    static constexpr int Frame_FrameWidth = 2;
    static constexpr int Frame_FrameRadius = 5;

    static constexpr int Layout_TopLevelMarginWidth = 10;
    static constexpr int Layout_ChildMarginWidth = 6;
    static constexpr int Layout_DefaultSpacing = 6;

    static constexpr int LineEdit_FrameWidth = 6;

    static constexpr int Menu_FrameWidth = 0;
    static constexpr int MenuItem_MarginWidth = 4;
    static constexpr int MenuItem_HighlightGap = 4;
    static constexpr int MenuItem_ExtraLeftMargin = 4;
    static constexpr int MenuItem_MarginHeight = 4;
    static constexpr int MenuItem_ItemSpacing = 2;
    static constexpr int MenuItem_AcceleratorSpace = 16;
    static constexpr int MenuItem_TextLeftMargin = 8;

    static constexpr int ComboBox_FrameWidth = 6;
    static constexpr int ComboBox_ItemMarginWidth = 3;

    static constexpr int SpinBox_FrameWidth = LineEdit_FrameWidth;
    static constexpr int SpinBox_ArrowButtonWidth = 20;

    static constexpr int GroupBox_TitleMarginWidth = 4;

    static constexpr int Button_MinWidth = 80;
    static constexpr int Button_MarginWidth = 6;
    static constexpr int Button_ItemSpacing = 4;

    static constexpr int ToolButton_MarginWidth = 6;
    static constexpr int ToolButton_ItemSpacing = 4;
    static constexpr int ToolButton_InlineIndicatorWidth = 12;

    static constexpr int MenuButton_IndicatorWidth = 20;

    static constexpr int CheckBox_Size = 20;
    static constexpr int CheckBox_FocusMarginWidth = 2;
    static constexpr int CheckBox_ItemSpacing = 4;
    static constexpr int CheckBox_Radius = Frame_FrameRadius - 1;

    static constexpr int MenuBarItem_MarginWidth = 10;
    static constexpr int MenuBarItem_MarginHeight = 6;

    static constexpr int ScrollBar_Extend = 21;
    static constexpr int ScrollBar_SliderWidth = 8;
    static constexpr int ScrollBar_MinSliderHeight = 20;
    static constexpr int ScrollBar_NoButtonHeight = 3;
    static constexpr int ScrollBar_SingleButtonHeight = ScrollBar_Extend;
    static constexpr int ScrollBar_DoubleButtonHeight = 2 * ScrollBar_Extend;

    static constexpr int ToolBar_FrameWidth = 0;
    static constexpr int ToolBar_HandleExtent = 10;
    static constexpr int ToolBar_HandleWidth = 6;
    static constexpr int ToolBar_SeparatorWidth = 8;
    static constexpr int ToolBar_ExtensionWidth = 20;
    static constexpr int ToolBar_ItemMargin = 6;
    static constexpr int ToolBar_ItemSpacing = 0;
    static constexpr int ToolBar_SeparatorVerticalMargin = 2;

    static constexpr int ProgressBar_BusyIndicatorSize = 14;
    static constexpr int ProgressBar_Thickness = 6;
    static constexpr int ProgressBar_ItemSpacing = 4;

    static constexpr int TitleBar_MarginWidth = 4;

    static constexpr int Slider_TickLength = 8;
    static constexpr int Slider_TickMarginWidth = 2;
    static constexpr int Slider_GrooveThickness = 6;
    static constexpr int Slider_ControlThickness = 20;

    static constexpr int TabBar_TabMarginHeight = 4;
    static constexpr int TabBar_TabMarginWidth = 8;
    static constexpr int TabBar_TabMinWidth = 80;
    static constexpr int TabBar_TabMinHeight = 30;
    static constexpr int TabBar_StaticTabMinHeight = 34;
    static constexpr int TabBar_TabItemSpacing = 8;
    static constexpr int TabBar_TabOverlap = 1;
    static constexpr int TabBar_BaseOverlap = 2;
    static constexpr int TabBar_ActiveEffectSize = 3;

    static constexpr int TabWidget_MarginWidth = 3;

    static constexpr int ToolBox_TabMinWidth = 80;
    static constexpr int ToolBox_TabItemSpacing = 4;
    static constexpr int ToolBox_TabMarginWidth = 8;

    static constexpr int ToolTip_FrameWidth = 3;

    static constexpr int Header_MarginWidth = 6;
    static constexpr int Header_ItemSpacing = 4;
    static constexpr int Header_ArrowSize = ArrowSize;

    static constexpr int ItemView_ArrowSize = ArrowSize;
    static constexpr int ItemView_ItemMarginLeft = 2;
    static constexpr int ItemView_ItemMarginRight = 2;
    static constexpr int ItemView_FirstItemTopMarginHeight = 2;
    static constexpr int ItemView_ItemMarginTop = 1;
    static constexpr int ItemView_ItemMarginBottom = 1;
    static constexpr int ItemView_ItemPaddingWidth = 2;
    static constexpr int ItemView_ItemPaddingHeight = 2;
    static constexpr int SidePanel_ItemMarginWidth = 4;

    static constexpr int Splitter_SplitterWidth = 1;

    static constexpr int Shadow_Overlap = 2;

    static constexpr qreal Blend_Value = 0.3;

    static constexpr qreal Focus_LightenColorValue = 110;
};
}
