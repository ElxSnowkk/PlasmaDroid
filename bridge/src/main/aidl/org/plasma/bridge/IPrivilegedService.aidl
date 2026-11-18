/*
 * This file belongs to the PlasmaDroid project.
 *
 * Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
 *
 * PlasmaDroid is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
 * or <https://www.gnu.org/licenses/gpl-3.0.html>.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 */

package org.plasma.bridge;

import android.view.Surface;

interface IPrivilegedService {
    void destroy() = 16777114;
    int uid() = 1;
    int createDisplay(String name, int width, int height, int dpi, in Surface surface, int flags) = 2;
    void releaseDisplay(int displayId) = 3;
    void resizeDisplay(int displayId, int width, int height, int dpi) = 4;
    void setDisplaySurface(int displayId, in Surface surface) = 5;
    String lastError() = 6;
    String exec(in String[] cmd, int timeoutMs) = 7;
    boolean injectMotion(int displayId, int action, float x, float y, long downTime, long eventTime, int source) = 10;
    oneway void injectTouch(int displayId, int action, int actionIndex, int source, int buttonState, long downTime, in long[] times, in int[] ids, in float[] xy) = 13;
    oneway void injectKey(int displayId, int keyCode, int action, int meta, long downTime, long eventTime) = 11;
    oneway void injectScroll(int displayId, float x, float y, float vScroll, float hScroll, long time) = 12;
    String inputStats() = 21;
    String bluetooth(String op, String address, int timeoutMs) = 20;
}
