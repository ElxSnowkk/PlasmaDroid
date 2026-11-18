
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
