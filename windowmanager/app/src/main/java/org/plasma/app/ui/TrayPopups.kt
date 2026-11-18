
package org.plasma.app.ui

import androidx.compose.ui.graphics.graphicsLayer
import android.provider.Settings
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.zIndex
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import org.plasma.app.android.PlasmaNotificationService
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.TrayPopup
import org.plasma.core.Keys
import org.plasma.core.SysSnapshot
import java.time.DayOfWeek
import java.time.LocalDate
import java.time.YearMonth
import java.time.format.TextStyle
import java.util.Locale

@Composable
fun BoxScope.PopupLayer(session: DesktopSession, panelDp: Int, panelTop: Boolean) {
    val pad = Modifier.padding(top = if (panelTop) (panelDp + 8).dp else 8.dp, bottom = if (panelTop) 8.dp else (panelDp + 8).dp, start = 8.dp, end = 8.dp)
    if (session.startMenuOpen)
        Kickoff(session, Modifier.align(if (panelTop) Alignment.TopStart else Alignment.BottomStart).then(pad).zIndex(1500f))
    val fx = LocalFx.current
    val pa = remember(session.tray) { androidx.compose.animation.core.Animatable(0f) }
    LaunchedEffect(session.tray) { session.glass.kick(fx.ms(Motion.LONG) + 80); pa.animateTo(1f, Motion.spec(fx, Motion.LONG)) }
    val dir = if (panelTop) -1f else 1f
    val tray = Modifier.align(if (panelTop) Alignment.TopEnd else Alignment.BottomEnd).then(pad).zIndex(1500f)
        .graphicsLayer { alpha = pa.value; translationY = (1f - pa.value) * 24f * dir }
    when (session.tray) {
        TrayPopup.CALENDAR -> PopupSurface(tray.width(300.dp)) { Box(Modifier.padding(12.dp)) { CalendarView() } }
        TrayPopup.NOTIFICATIONS -> NotificationsPopup(tray)
        TrayPopup.SYSTEM -> PopupSurface(tray.width(300.dp)) { Box(Modifier.padding(12.dp)) { SystemMonitorView(session) } }
        TrayPopup.NETWORK -> TrayPanelPopup(tray) { NetworkPanel(session) }
        TrayPopup.BLUETOOTH -> TrayPanelPopup(tray) { BluetoothPanel(session) }
        TrayPopup.VOLUME -> TrayPanelPopup(tray) { VolumePanel(session) }
        TrayPopup.BRIGHTNESS -> TrayPanelPopup(tray) { BrightnessPanel(session) }
        TrayPopup.NONE -> {}
    }
}

@Composable
private fun NotificationsPopup(modifier: Modifier) {
    val ctx = LocalContext.current
    val b = LocalBreeze.current
    val list by PlasmaNotificationService.active.collectAsState()
    val connected = PlasmaNotificationService.instance != null
    PopupSurface(modifier.width(340.dp).heightIn(max = 480.dp)) {
        Row(Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            T("Notificações", 15.sp, weight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
            if (connected && list.isNotEmpty()) BreezeButton("Limpar tudo", { runCatching { PlasmaNotificationService.instance?.cancelAllNotifications() } })
        }
        if (!connected) {
            T("Para exibir as notificações do Android, conceda o acesso a notificações a este app.", 12.sp, b.dim, modifier = Modifier.padding(12.dp))
            BreezeButton("Conceder acesso", {
                ctx.startActivity(android.content.Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
            }, Modifier.padding(12.dp), primary = true)
        } else if (list.isEmpty()) T("Sem notificações", 13.sp, b.dim, modifier = Modifier.padding(16.dp))
        else LazyColumn {
            items(list, key = { it.key }) { n ->
                val ex = n.notification.extras
                val title = ex.getCharSequence("android.title")?.toString().orEmpty()
                val text = ex.getCharSequence("android.text")?.toString().orEmpty()
                val app = runCatching { ctx.packageManager.getApplicationLabel(ctx.packageManager.getApplicationInfo(n.packageName, 0)).toString() }.getOrDefault(n.packageName)
                Column(Modifier.fillMaxWidth().clickable { runCatching { n.notification.contentIntent?.send() } }.padding(horizontal = 12.dp, vertical = 8.dp)) {
                    T(app, 11.sp, b.accent, maxLines = 1)
                    if (title.isNotBlank()) T(title, 13.sp, weight = FontWeight.SemiBold, maxLines = 1)
                    if (text.isNotBlank()) T(text, 12.sp, b.dim, maxLines = 2)
                }
            }
        }
    }
}

@Composable
fun CalendarView() {
    val b = LocalBreeze.current
    var ym by remember { mutableStateOf(YearMonth.now()) }
    val today = LocalDate.now()
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.clickable { ym = ym.minusMonths(1) }.padding(8.dp)) { BreezeIcon("chevL", 16.dp) }
            T("${ym.month.getDisplayName(TextStyle.FULL, Locale("pt", "BR")).replaceFirstChar { it.uppercase() }} ${ym.year}", 14.sp,
                weight = FontWeight.SemiBold, modifier = Modifier.weight(1f), align = TextAlign.Center)
            Box(Modifier.clickable { ym = ym.plusMonths(1) }.padding(8.dp)) { BreezeIcon("chevR", 16.dp) }
        }
        Row(Modifier.fillMaxWidth()) {
            for (i in 0 until 7) {
                val d = DayOfWeek.of(((i + 6) % 7) + 1)
                T(d.getDisplayName(TextStyle.NARROW, Locale("pt", "BR")), 11.sp, b.dim, modifier = Modifier.weight(1f), align = TextAlign.Center)
            }
        }
        val first = ym.atDay(1)
        val offset = first.dayOfWeek.value % 7
        val cells = offset + ym.lengthOfMonth()
        for (row in 0 until (cells + 6) / 7) {
            Row(Modifier.fillMaxWidth().padding(vertical = 1.dp)) {
                for (col in 0 until 7) {
                    val n = row * 7 + col - offset + 1
                    val isToday = n in 1..ym.lengthOfMonth() && ym.atDay(n) == today
                    Box(Modifier.weight(1f).aspectRatio(1.2f).clip(RoundedCornerShape(4.dp)).background(if (isToday) b.accent else androidx.compose.ui.graphics.Color.Transparent),
                        contentAlignment = Alignment.Center) {
                        if (n in 1..ym.lengthOfMonth()) T("$n", 12.sp, if (isToday) androidx.compose.ui.graphics.Color.White else b.text)
                    }
                }
            }
        }
    }
}

@Composable
fun SystemMonitorView(session: DesktopSession) {
    val app = session.app
    val b = LocalBreeze.current
    val snap by produceState<SysSnapshot?>(null) {
        while (true) { value = withContext(Dispatchers.IO) { app.stats.snapshot() }; delay(1000) }
    }
    val st = app.settings; st.watch()
    Column(Modifier.fillMaxWidth()) {
        val s = snap
        if (s == null) { T("Lendo métricas…", 12.sp, b.dim); return@Column }
        val used = (s.memTotalMb - s.memAvailMb).coerceAtLeast(0)
        Meter("Memória do sistema", used / s.memTotalMb.coerceAtLeast(1).toFloat(), "$used / ${s.memTotalMb} MB")
        Meter("CPU deste app", (s.appCpuPct / (100f * s.cpuCores)).coerceIn(0f, 1f), "%.0f%% de 1 núcleo".format(s.appCpuPct))
        T("Memória do app: ${s.appPssMb} MB", 12.sp, modifier = Modifier.padding(top = 4.dp))
        T("Bateria: ${if (s.batteryPct >= 0) "${s.batteryPct}%" else "n/d"}${if (s.charging) " (carregando)" else ""}" +
            (s.batteryTempC?.let { " · %.1f °C".format(it) } ?: ""), 12.sp)
        T("GPU: ${s.gpuBusyPct?.let { "$it% em uso" } ?: "uso indisponível (sem acesso do app ao driver)"}", 12.sp)
        T("Backend gráfico verificado: ${st.string("render.active", "…")}", 12.sp, b.dim)
    }
}

@Composable
private fun Meter(label: String, fraction: Float, detail: String) {
    val b = LocalBreeze.current
    Column(Modifier.padding(vertical = 3.dp)) {
        Row { T(label, 12.sp, modifier = Modifier.weight(1f)); T(detail, 11.sp, b.dim) }
        Box(Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)).background(b.hover)) {
            Box(Modifier.fillMaxWidth(fraction.coerceIn(0f, 1f)).fillMaxHeight().background(b.accent))
        }
    }
}
