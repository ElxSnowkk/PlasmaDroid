
package org.plasma.x11

import android.view.Surface

object NativeX11Bridge {
    init { System.loadLibrary("plasma_x11") }

    external fun nativeStart(socketDir: String, display: Int, width: Int, height: Int): Long
    external fun nativeLastError(): String
    external fun nativeStop(handle: Long)
    external fun nativeWindowListJson(handle: Long): String
    external fun nativePointerMotion(handle: Long, xid: Int, x: Int, y: Int, state: Int)
    external fun nativePointerButton(handle: Long, xid: Int, x: Int, y: Int, button: Int, pressed: Boolean, state: Int)
    external fun nativeKey(handle: Long, xid: Int, linuxKeycode: Int, pressed: Boolean, state: Int)
    external fun nativeFocus(handle: Long, xid: Int)
    external fun nativeClose(handle: Long, xid: Int)
    external fun nativeAttachSurface(handle: Long, xid: Int, surface: Surface)
    external fun nativeResizeWindow(handle: Long, xid: Int, width: Int, height: Int)
    external fun nativeDetachSurface(handle: Long, xid: Int)
    external fun nativeSetScreenSize(handle: Long, width: Int, height: Int)
}
