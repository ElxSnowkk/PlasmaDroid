
package org.plasma.wayland

import android.view.Surface

object NativeWaylandBridge {
    init { System.loadLibrary("piperella_compositor") }

    external fun nativeStartHeadless(
        socketDir: String,
        dataDir: String,
        xkbConfigRoot: String,
        width: Int,
        height: Int,
        refreshMhz: Int,
    ): Long

    external fun nativeAttachSurface(handle: Long, surface: Surface): Boolean
    external fun nativeAttachClientWindow(handle: Long, windowId: Long, surface: Surface, width: Int, height: Int): Boolean
    external fun nativeDetachClientWindow(handle: Long, windowId: Long)
    external fun nativeResizeClientWindow(handle: Long, windowId: Long, width: Int, height: Int)
    external fun nativeFocusToplevel(handle: Long, windowId: Long)
    external fun nativeCloseToplevel(handle: Long, windowId: Long)
    external fun nativeRegisterImeBridge(handle: Long, bridge: Any)
    external fun nativeImeCommitText(handle: Long, text: String)
    external fun nativeImeSetPreeditText(handle: Long, text: String, cursorBegin: Int, cursorEnd: Int)
    external fun nativeUnregisterImeBridge(handle: Long)
    external fun nativeDetachSurface(handle: Long)
    external fun nativeResize(handle: Long, width: Int, height: Int)
    external fun nativeRenderFrame(handle: Long, frameTimeNs: Long): Boolean
    external fun nativeGetWindowList(handle: Long): String
    external fun nativeDispatchMotion(
        handle: Long, source: Int, action: Int, pointerId: Int,
        x: Float, y: Float, buttonState: Int, eventTimeNs: Long,
    )
    external fun nativeDispatchKey(handle: Long, scanCode: Int, action: Int, eventTimeNs: Long, metaState: Int)
    external fun nativeDispatchScroll(handle: Long, dx: Float, dy: Float, eventTimeNs: Long)
    external fun nativeSetTrackpad(handle: Long, enabled: Boolean)
    external fun nativeStop(handle: Long)
}
