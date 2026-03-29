/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file ferroneer_welcome_gui.cpp Ferroneer welcome / quick-start guide window. */

#include "stdafx.h"
#include "ferroneer_welcome_gui.h"
#include "window_gui.h"
#include "strings_func.h"
#include "gfx_func.h"
#include "window_func.h"
#include "zoom_func.h"

#include "table/strings.h"

#include "safeguards.h"

/** Widget IDs for the Ferroneer welcome window. */
enum FerroneerWelcomeWidgets : WidgetID {
	WID_FW_CAPTION,
	WID_FW_BACKGROUND,
	WID_FW_TEXT,
	WID_FW_DISMISS,
	WID_FW_NEVER,
};

static bool _ferroneer_welcome_suppressed = false;

struct FerroneerWelcomeWindow : Window {
	FerroneerWelcomeWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(0);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_FW_DISMISS:
				this->Close();
				break;

			case WID_FW_NEVER:
				_ferroneer_welcome_suppressed = true;
				this->Close();
				break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_FW_TEXT) return;

		Rect tr = r.Shrink(WidgetDimensions::scaled.frametext);
		int y = tr.top;
		int line_height = GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_normal;

		y = DrawStringMultiLine(tr.left, tr.right, y, y + line_height * 2, STR_FERRONEER_WELCOME_GUIDE, TC_BLACK);
		y += WidgetDimensions::scaled.vsep_wide;
		y = DrawStringMultiLine(tr.left, tr.right, y, y + line_height, STR_FERRONEER_WELCOME_STEP1, TC_BLACK);
		y = DrawStringMultiLine(tr.left, tr.right, y, y + line_height, STR_FERRONEER_WELCOME_STEP1_HINT, TC_BLACK);
		y = DrawStringMultiLine(tr.left, tr.right, y, y + line_height, STR_FERRONEER_WELCOME_STEP1_TIP, TC_BLACK);
		y += WidgetDimensions::scaled.vsep_normal;
		y = DrawStringMultiLine(tr.left, tr.right, y, y + line_height, STR_FERRONEER_WELCOME_STEP2, TC_BLACK);
		y += WidgetDimensions::scaled.vsep_normal;
		y = DrawStringMultiLine(tr.left, tr.right, y, y + line_height, STR_FERRONEER_WELCOME_STEP3, TC_BLACK);
		y = DrawStringMultiLine(tr.left, tr.right, y, y + line_height, STR_FERRONEER_WELCOME_STEP3_HINT, TC_BLACK);
		y = DrawStringMultiLine(tr.left, tr.right, y, y + line_height, STR_FERRONEER_WELCOME_STEP3_TIP, TC_BLACK);
		y += WidgetDimensions::scaled.vsep_normal;
		DrawStringMultiLine(tr.left, tr.right, y, y + line_height, STR_FERRONEER_WELCOME_STEP4, TC_BLACK);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget == WID_FW_TEXT) {
			int line_height = GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_normal;
			/* 10 lines of text + 4 vertical gaps + frame padding */
			size.height = line_height * 10 + WidgetDimensions::scaled.vsep_wide * 4 + WidgetDimensions::scaled.frametext.Vertical();
			size.width = std::max(size.width, (uint)ScaleGUITrad(350));
		}
	}
};

static constexpr NWidgetPart _nested_ferroneer_welcome_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_FW_CAPTION), SetStringTip(STR_FERRONEER_WELCOME_TITLE, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_FW_BACKGROUND),
		NWidget(WWT_EMPTY, INVALID_COLOUR, WID_FW_TEXT), SetMinimalSize(350, 200), SetPadding(WidgetDimensions::unscaled.frametext),
	EndContainer(),
	NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_FW_DISMISS), SetStringTip(STR_FERRONEER_WELCOME_DISMISS), SetFill(1, 0),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_FW_NEVER), SetStringTip(STR_FERRONEER_WELCOME_NEVER), SetFill(1, 0),
	EndContainer(),
};

static WindowDesc _ferroneer_welcome_desc(
	WDP_CENTER, {}, 0, 0,
	WC_FERRONEER_WELCOME, WC_NONE,
	{},
	_nested_ferroneer_welcome_widgets
);

void ShowFerroneerWelcomeWindow()
{
	if (_ferroneer_welcome_suppressed) return;
	if (BringWindowToFrontById(WC_FERRONEER_WELCOME, 0) != nullptr) return;
	new FerroneerWelcomeWindow(_ferroneer_welcome_desc);
}
