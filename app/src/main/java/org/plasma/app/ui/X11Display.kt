
package org.plasma.app.ui

import android.graphics.PixelFormat
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.viewinterop.AndroidView
import org.plasma.app.session.DesktopSession
import org.plasma.core.Keys
import java.io.File

@Composable
fun X11ServerHost(session: DesktopSession) {
    if (session.displayId != android.view.Display.DEFAULT_DISPLAY) return
    val server = session.app.x11
    val list by server.windows.collectAsState()
    val st = session.app.settings
    val x11Mode = st.string(Keys.GFX_SERVER, "wayland") == "x11"
    val ctx = LocalContext.current
    val cfg = LocalConfiguration.current
    val dm = ctx.resources.displayMetrics
    val screenW = dm.widthPixels
    val screenH = dm.heightPixels
    LaunchedEffect(screenW, screenH, cfg.orientation, cfg.screenWidthDp, cfg.screenHeightDp) {
        server.setScreenSize(screenW, screenH)
    }
    LaunchedEffect(x11Mode) { if (x11Mode) server.ensure(File(session.app.debian.paths.rootfs, "tmp"), width = screenW, height = screenH) }
    LaunchedEffect(list) { session.syncX11Windows(list) }
}

@Composable
fun X11ClientWindowContent(session: DesktopSession, xid: Long) {
    val server = session.app.x11
    AndroidView(
        modifier = Modifier.fillMaxSize().background(Color(0xFF101418)),
        factory = { context ->
            object : SurfaceView(context) {
                override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
                    server.key(xid, event)
                    return true
                }
                override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
                    server.key(xid, event)
                    return true
                }
            }.apply {
                isFocusable = true
                isFocusableInTouchMode = true
                holder.setFormat(PixelFormat.OPAQUE)
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) {
                        server.attachSurface(xid, holder.surface)
                        requestFocus()
                    }
                    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                        server.resizeWindow(xid, width, height)
                    }
                    override fun surfaceDestroyed(holder: SurfaceHolder) {
                        server.detachSurface(xid)
                    }
                })
                setOnTouchListener { v, e ->
                    v.requestFocus()
                    server.pointer(xid, e, v.width, v.height)
                }
            }
        },
        update = { },
    )
}
