// Parte do projeto PlasmaDroid.

package org.plasma.app.ui

import android.graphics.PixelFormat
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.FrameLayout
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.viewinterop.AndroidView
import org.plasma.app.session.DesktopSession
import org.plasma.wayland.WaylandImeInputView

private const val WAYLAND_TOUCH = 0x00001002
private const val WAYLAND_MOUSE = 0x00002002

@Composable
fun WaylandClientWindowContent(session: DesktopSession, waylandId: Long) {
    val manager = session.app.wayland

    AndroidView(
        modifier = Modifier.fillMaxSize().background(Color(0xFF101418)),
        factory = { context ->
            FrameLayout(context).apply {
                val output = object : SurfaceView(context) {
                    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
                        manager.focusClientWindow(waylandId)
                        manager.dispatchKey(event)
                        return true
                    }
                    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
                        manager.dispatchKey(event)
                        return true
                    }
                }.apply {
                    isFocusable = true
                    isFocusableInTouchMode = true
                    holder.setFormat(PixelFormat.OPAQUE)
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            manager.attachClientSurface(waylandId, holder.surface, width, height)
                            requestFocus()
                        }
                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                            manager.resizeClientSurface(waylandId, width, height)
                        }
                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            manager.detachClientSurface(waylandId)
                        }
                    })
                    setOnTouchListener { _, event ->
                        val now = System.nanoTime()
                        val source = if ((event.source and WAYLAND_MOUSE) == WAYLAND_MOUSE) WAYLAND_MOUSE else WAYLAND_TOUCH
                        when (event.actionMasked) {
                            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                                val i = event.actionIndex
                                manager.dispatchClientMotion(waylandId, source, event.actionMasked,
                                    event.getPointerId(i), event.getX(i), event.getY(i), event.buttonState, now)
                                requestFocus()
                            }
                            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                                val i = event.actionIndex
                                manager.dispatchClientMotion(waylandId, source, event.actionMasked,
                                    event.getPointerId(i), event.getX(i), event.getY(i), event.buttonState, now)
                            }
                            MotionEvent.ACTION_MOVE -> for (i in 0 until event.pointerCount) {
                                manager.dispatchClientMotion(waylandId, source, MotionEvent.ACTION_MOVE,
                                    event.getPointerId(i), event.getX(i), event.getY(i), event.buttonState, now)
                            }
                            MotionEvent.ACTION_CANCEL -> manager.dispatchClientMotion(waylandId,
                                source, MotionEvent.ACTION_CANCEL, 0, event.x, event.y, event.buttonState, now)
                        }
                        true
                    }
                    setOnGenericMotionListener { _, event ->
                        val now = System.nanoTime()
                        when (event.actionMasked) {
                            MotionEvent.ACTION_HOVER_MOVE -> {
                                manager.dispatchClientMotion(waylandId, WAYLAND_MOUSE, 7, 0, event.x, event.y, event.buttonState, now); true
                            }
                            MotionEvent.ACTION_HOVER_ENTER -> {
                                manager.dispatchClientMotion(waylandId, WAYLAND_MOUSE, 9, 0, event.x, event.y, event.buttonState, now); true
                            }
                            MotionEvent.ACTION_HOVER_EXIT -> {
                                manager.dispatchClientMotion(waylandId, WAYLAND_MOUSE, 10, 0, event.x, event.y, event.buttonState, now); true
                            }
                            MotionEvent.ACTION_SCROLL -> {
                                manager.focusClientWindow(waylandId)
                                manager.dispatchScroll(event.getAxisValue(MotionEvent.AXIS_HSCROLL),
                                    -event.getAxisValue(MotionEvent.AXIS_VSCROLL), now)
                                true
                            }
                            else -> false
                        }
                    }
                }
                addView(output, FrameLayout.LayoutParams(-1, -1))

                val ime = WaylandImeInputView(context,
                    commit = manager::commitImeText,
                    preedit = manager::setImePreedit,
                    key = manager::dispatchKey,
                )
                manager.registerImeView(waylandId, ime)
                addView(ime, FrameLayout.LayoutParams(1, 1))
                tag = Pair(waylandId, ime)
            }
        },
        update = { layout ->
            (layout.tag as? Pair<*, *>)?.second?.let { ime ->
                if (ime is WaylandImeInputView) manager.registerImeView(waylandId, ime)
            }
        },
    )

}
