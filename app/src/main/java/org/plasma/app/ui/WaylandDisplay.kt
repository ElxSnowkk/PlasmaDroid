// Parte do projeto PlasmaDroid.

package org.plasma.app.ui

import android.graphics.PixelFormat
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import org.plasma.app.session.DesktopSession

@Composable
fun WaylandDisplayContent(session: DesktopSession) {
    val manager = session.app.wayland

    AndroidView(
        modifier = Modifier.size(1.dp).alpha(0.01f),
        factory = { context ->
            SurfaceView(context).apply {
                holder.setFormat(PixelFormat.OPAQUE)
                holder.addCallback(object : SurfaceHolder.Callback {
                    override fun surfaceCreated(holder: SurfaceHolder) {
                        manager.attachSurface(holder.surface, 1280, 720)
                    }
                    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                        manager.resizeSurface(1280, 720)
                    }
                    override fun surfaceDestroyed(holder: SurfaceHolder) {
                        manager.detachSurface()
                    }
                })
            }
        },
        update = { },
    )
}
