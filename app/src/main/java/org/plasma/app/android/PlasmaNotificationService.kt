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

package org.plasma.app.android

import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class PlasmaNotificationService : NotificationListenerService() {
    companion object {
        private val _active = MutableStateFlow<List<StatusBarNotification>>(emptyList())
        val active: StateFlow<List<StatusBarNotification>> = _active.asStateFlow()
        @Volatile var instance: PlasmaNotificationService? = null
    }

    private fun refresh() {
        _active.value = runCatching { activeNotifications.toList() }.getOrDefault(emptyList())
    }
    override fun onListenerConnected() { instance = this; refresh() }
    override fun onListenerDisconnected() { instance = null; _active.value = emptyList() }
    override fun onNotificationPosted(sbn: StatusBarNotification?) = refresh()
    override fun onNotificationRemoved(sbn: StatusBarNotification?) = refresh()
}
