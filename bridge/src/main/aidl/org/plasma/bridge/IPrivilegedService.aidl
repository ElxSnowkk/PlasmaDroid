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
