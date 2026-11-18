
package org.plasma.bridge

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import androidx.core.graphics.drawable.toBitmap

data class AndroidApp(val pkg: String, val activity: String, val label: String) {
    val component get() = "$pkg/$activity"
}

class AppCatalog(private val ctx: Context) {
    fun load(): List<AndroidApp> {
        val pm = ctx.packageManager
        val i = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        return pm.queryIntentActivities(i, 0)
            .map { AndroidApp(it.activityInfo.packageName, it.activityInfo.name, it.loadLabel(pm).toString()) }
            .filter { it.pkg != ctx.packageName }
            .distinctBy { it.pkg }
            .sortedBy { it.label.lowercase() }
    }

    fun icon(a: AndroidApp, px: Int): Bitmap? =
        runCatching { ctx.packageManager.getActivityIcon(ComponentName(a.pkg, a.activity)).toBitmap(px, px) }.getOrNull()

    fun launchIntent(a: AndroidApp): Intent =
        Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            .setComponent(ComponentName(a.pkg, a.activity))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_MULTIPLE_TASK)
}
