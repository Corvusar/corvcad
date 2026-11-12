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
// CorvCAD iOS stand-in for KiCad 10.0.6 pcbnew/dialogs/dialog_plot.h. Upstream's DIALOG_PLOT
// derives from DIALOG_PLOT_BASE, whose wxFormBuilder layout uses wxStaticBoxSizer, which
// wxWidgets' iOS port does not have. In the board slice only pcbnew/pcbplot.cpp includes the
// header, and it uses nothing from the dialog: the plot functions the fabrication outputs
// call (StartPlotBoard, PlotBoardLayers) live in pcbplot.cpp itself. This keeps upstream's
// other includes, so pcbplot.cpp sees the same declarations as on the desktop.

#pragma once

#include <board.h>
#include <pcb_plot_params.h>
#include <widgets/unit_binder.h>
