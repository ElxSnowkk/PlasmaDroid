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

package org.plasma.bridge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.IBinder
import android.view.Surface
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.plasma.core.PlasmaLog

object ProjectionHolder {
    @Volatile var projection: MediaProjection? = null
    private val _active = MutableStateFlow(false)
    val active: StateFlow<Boolean> = _active.asStateFlow()
    internal fun setActive(v: Boolean) { _active.value = v }

    fun mirror(ctx: Context, surface: Surface, w: Int, h: Int, dpi: Int): VirtualDisplay? {
        val p = projection ?: return null
        return try {
            p.createVirtualDisplay("plasma-capture", w, h, dpi, DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, surface, null, null)
        } catch (t: Throwable) { PlasmaLog.e("projection", "createVirtualDisplay falhou", t); null }
    }
}

class ProjectionService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel("capture", "Captura de tela", NotificationManager.IMPORTANCE_LOW))
        val n = Notification.Builder(this, "capture")
            .setContentTitle("KDE Plasma Android Desktop")
            .setContentText("Captura de tela autorizada pelo usuário")
            .setSmallIcon(android.R.drawable.ic_menu_camera).setOngoing(true).build()
        if (Build.VERSION.SDK_INT >= 29) startForeground(77, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        else startForeground(77, n)
        val code = intent?.getIntExtra("code", 0) ?: 0
        val data = intent?.getParcelableExtra<Intent>("data")
        if (data == null) { stopSelf(); return START_NOT_STICKY }
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val p = mpm.getMediaProjection(code, data)
        p.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() { ProjectionHolder.projection = null; ProjectionHolder.setActive(false); stopSelf() }
        }, null)
        ProjectionHolder.projection = p
        ProjectionHolder.setActive(true)
        PlasmaLog.i("projection", "MediaProjection autorizada")
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        ProjectionHolder.projection?.stop()
        ProjectionHolder.projection = null; ProjectionHolder.setActive(false)
    }
}
