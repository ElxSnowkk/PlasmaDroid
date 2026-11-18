
package org.plasma.app.ui

import androidx.compose.foundation.*
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsHoveredAsState
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.plasma.app.R
import org.plasma.app.android.PlasmaNotificationService
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.MenuItem
import org.plasma.app.session.MenuState
import org.plasma.app.session.TrayPopup
import org.plasma.bridge.ShizukuState
import org.plasma.core.Keys
import org.plasma.wm.WmState
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

@Composable
fun Panel(session: DesktopSession, wm: WmState, heightDp: Int, floating: Boolean, opaque: Boolean, modifier: Modifier) {
    val b = LocalBreeze.current
    val fx = LocalFx.current
    val st = session.app.settings
    val kind by animateFloatAsState(if (opaque) 1.8f else 1f, Motion.spec(fx, Motion.VERY_LONG), label = "panelKind")
    val margin = if (floating) st.int(Keys.PANEL_MARGIN, U.DEFAULT_PANEL_MARGIN_DP).coerceIn(0, 48).dp else 0.dp
    val radius = if (floating) st.int(Keys.PANEL_RADIUS, U.DEFAULT_PANEL_RADIUS_DP).coerceIn(0, 32).dp else 0.dp
    val widthFrac = if (floating) st.int(Keys.PANEL_WIDTH_PCT, U.DEFAULT_PANEL_WIDTH_PCT).coerceIn(40, 100) / 100f else 1f
    LaunchedEffect(opaque) { session.glass.kick(fx.ms(Motion.VERY_LONG + 60)) }
    BoxWithConstraints(modifier.fillMaxWidth()) {

    val portrait = maxHeight > maxWidth
    val trayMode = st.string(Keys.PANEL_TRAY_MODE, "auto")
    val collapseTray = trayMode == "always" || (trayMode == "auto" && portrait)
    Box(Modifier.fillMaxWidth().padding(margin)) {
        Row(
            Modifier.fillMaxWidth(widthFrac).height(heightDp.dp).align(Alignment.Center)
                .glass(b.panel, radius, shadow = if (floating) 16.dp else 0.dp, shadowAlpha = .45f, kind = kind, z = 10f)
                .border(1.dp, b.line.copy(alpha = if (floating) .55f else 0f), RoundedCornerShape(radius))
                .onSecondaryClick { p ->
                    val top = st.bool(Keys.PANEL_TOP, false)
                    session.menu = MenuState(p.x, p.y, listOf(
                        MenuItem(if (top) "Mover o painel para baixo" else "Mover o painel para cima") { st.put(Keys.PANEL_TOP, !top) },
                        MenuItem(if (floating) "Painel encaixado" else "Painel flutuante") { st.put(Keys.PANEL_FLOATING, !floating) },
                        MenuItem("Aumentar o painel", heightDp < 72) { st.put(Keys.PANEL_H, heightDp + 6) },
                        MenuItem("Diminuir o painel", heightDp > 34) { st.put(Keys.PANEL_H, heightDp - 6) },
                        MenuItem("Configurar o painel…") { session.openTool("settings", "appearance") }))
                },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            KickoffButton(session, heightDp)
            Pager(session, wm)
            TaskManager(session, wm, heightDp, Modifier.weight(1f))
            if (collapseTray) TrayOverflowButton(session) else SystemTray(session)
            ClockButton(session)
            Box(Modifier.fillMaxHeight().width(1.dp).background(b.line.copy(alpha = .6f)))
            val sd by animateFloatAsState(if (wm.showingDesktop) 1f else 0f, Motion.spec(fx, Motion.LONG), label = "showDesk")
            Box(Modifier.fillMaxHeight().width(10.dp).drawBehind { drawRect(b.accent.copy(alpha = .75f * sd)) }
                .clickable(indication = null, interactionSource = remember { MutableInteractionSource() }) { session.closePopups(); session.wm.toggleShowDesktop() })
        }
    }
    }
}

@Composable
private fun KickoffButton(session: DesktopSession, heightDp: Int) {
    val b = LocalBreeze.current
    val fx = LocalFx.current
    val src = remember { MutableInteractionSource() }
    val hover by src.collectIsHoveredAsState()
    val a by animateFloatAsState(if (session.startMenuOpen) 1f else if (hover) .55f else 0f, Motion.spec(fx, Motion.SHORT), label = "kb")
    val sc by animateFloatAsState(if (session.startMenuOpen) .92f else 1f, Motion.spec(fx, Motion.SHORT), label = "kbs")
    Box(Modifier.fillMaxHeight().aspectRatio(1f).padding(4.dp).clip(RoundedCornerShape(6.dp))
        .drawBehind { drawRect(b.accent.copy(alpha = .32f * a)) }
        .hoverable(src).clickable(interactionSource = src, indication = null) { val o = !session.startMenuOpen; session.closePopups(); session.startMenuOpen = o },
        contentAlignment = Alignment.Center) {
        Image(painterResource(R.drawable.ic_plasma_glyph), "Menu de aplicativos", Modifier.size((heightDp * 0.62f / 1.5f).dp).graphicsLayer { scaleX = sc; scaleY = sc })
    }
}

@Composable
private fun Pager(session: DesktopSession, wm: WmState) {
    val b = LocalBreeze.current
    val wa = wm.workArea
    Row(Modifier.padding(horizontal = 4.dp)) {
        for (i in 0 until wm.desktopCount) {
            Box(Modifier.padding(horizontal = 2.dp, vertical = 8.dp).fillMaxHeight().aspectRatio((wa.w / wa.h.coerceAtLeast(1f)).coerceIn(1f, 2.2f))
                .clip(RoundedCornerShape(3.dp)).background(if (i == wm.desktop) b.accent.copy(alpha = .30f) else b.hover)
                .border(1.dp, if (i == wm.desktop) b.accent else b.line, RoundedCornerShape(3.dp))
                .clickable { session.closePopups(); session.wm.switchDesktop(i) }) {
                Canvas(Modifier.fillMaxSize()) {
                    wm.windows.filter { it.desktop == i && !it.minimized }.forEach { w ->
                        val sx = size.width / wa.w.coerceAtLeast(1f); val sy = size.height / wa.h.coerceAtLeast(1f)
                        drawRect(b.text.copy(alpha = .55f), Offset((w.frame.x - wa.x) * sx, (w.frame.y - wa.y) * sy),
                            Size(w.frame.w * sx, w.frame.h * sy), style = Stroke(1.dp.toPx()))
                    }
                }
            }
        }
    }
}

@Composable
private fun TaskManager(session: DesktopSession, wm: WmState, heightDp: Int, modifier: Modifier) {
    val st = session.app.settings
    st.watch()
    val apps by session.app.apps.collectAsState()
    val pinned = st.list(Keys.PINNED, "tool:files,tool:terminal,tool:settings")
    val mine = wm.windows.filter { it.desktop == wm.desktop }
    val groups = mine.groupBy { it.appId }
    val ids = pinned + groups.keys.filter { it !in pinned }
    Row(modifier.fillMaxHeight().horizontalScroll(rememberScrollState()), verticalAlignment = Alignment.CenterVertically) {
        ids.forEach { id ->
            val wins = groups[id].orEmpty()
            val active = wins.any { it.id == wm.focusedId }
            val b = LocalBreeze.current
            val fx = LocalFx.current
            val src = remember { MutableInteractionSource() }
            val hover by src.collectIsHoveredAsState()
            val bgA by animateFloatAsState(if (active) 1f else if (hover) .5f else 0f, Motion.spec(fx, Motion.SHORT), label = "taskBg")
            val ind by animateFloatAsState(if (active) 1f else .38f, Motion.spec(fx, Motion.LONG), label = "taskInd")
            Box(
                Modifier.fillMaxHeight().width((heightDp + 6).dp).padding(horizontal = 2.dp, vertical = 3.dp).clip(RoundedCornerShape(6.dp))
                    .drawBehind { drawRect(if (active) b.accent.copy(alpha = .26f * bgA) else b.text.copy(alpha = .12f * bgA)) }
                    .hoverable(src)
                    .pointerInput(id, wins.map { it.id }, wm.focusedId) {
                        detectTapGestures(onLongPress = { p -> taskMenu(session, id, wins.map { it.id }, pinned, p) }) {
                            session.closePopups()
                            when {
                                wins.isEmpty() -> openById(session, id)
                                wins.size == 1 -> session.wm.toggleTask(wins[0].id)
                                else -> {
                                    val i = wins.indexOfFirst { it.id == wm.focusedId }
                                    session.wm.focus(wins[(i + 1) % wins.size].id)
                                }
                            }
                        }
                    }.onSecondaryClick { p -> taskMenu(session, id, wins.map { it.id }, pinned, p) },
                contentAlignment = Alignment.Center,
            ) {
                EntryIcon(id, (heightDp * 0.65f).dp)
                if (wins.isNotEmpty()) Box(
                    Modifier.align(Alignment.BottomCenter).padding(bottom = 1.dp).height(3.dp)
                        .fillMaxWidth(ind * .8f).clip(RoundedCornerShape(2.dp)).background(b.accent))
                if (wins.size > 1) T("${wins.size}", 10.sp, b.text, FontWeight.Bold, Modifier.align(Alignment.TopEnd).padding(4.dp))
            }
        }
    }
}

private fun taskMenu(session: DesktopSession, id: String, winIds: List<Int>, pinned: List<String>, p: Offset) {
    val st = session.app.settings
    val isPinned = id in pinned
    session.menu = MenuState(p.x, p.y - 120f, buildList {
        add(MenuItem(if (winIds.isEmpty()) "Abrir" else "Abrir nova janela") { openById(session, id) })
        if (isPinned) add(MenuItem("Desafixar da barra de tarefas") { st.putList(Keys.PINNED, pinned - id) })
        else add(MenuItem("Fixar na barra de tarefas") { st.putList(Keys.PINNED, pinned + id) })
        if (winIds.isNotEmpty()) {
            add(MenuItem("", separator = true))
            for (n in 0 until session.wm.state.value.desktopCount)
                add(MenuItem("Mover para a área de trabalho ${n + 1}", n != session.wm.state.value.desktop) { winIds.forEach { w -> session.wm.moveToDesktop(w, n) } })
            add(MenuItem("", separator = true))
            add(MenuItem(if (winIds.size > 1) "Fechar tudo" else "Fechar") { winIds.forEach { session.closeWindow(it) } })
        }
    })
}

@Composable
private fun SystemTray(session: DesktopSession) {
    Row(Modifier.fillMaxHeight().padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) { TrayIcons(session) }
}

@Composable
private fun TrayOverflowButton(session: DesktopSession) {
    val b = LocalBreeze.current
    val notes by PlasmaNotificationService.active.collectAsState()
    val open = session.tray == TrayPopup.OVERFLOW
    TrayItem(onClick = { val o = session.tray != TrayPopup.OVERFLOW; session.closePopups(); if (o) session.tray = TrayPopup.OVERFLOW }) {
        Box {
            BreezeIcon("chevU", 18.dp, b.text, Modifier.graphicsLayer { rotationZ = if (open) 180f else 0f })
            if (notes.isNotEmpty()) Box(Modifier.align(Alignment.TopEnd).size(7.dp).clip(RoundedCornerShape(4.dp)).background(b.accent))
        }
    }
}

val LocalTrayTall = compositionLocalOf { true }

@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun TrayOverflowContent(session: DesktopSession) {
    CompositionLocalProvider(LocalTrayTall provides false) {
        FlowRow(Modifier.padding(8.dp).widthIn(max = 260.dp), maxItemsInEachRow = 4, verticalArrangement = Arrangement.spacedBy(2.dp)) { TrayIcons(session) }
    }
}

@Composable
private fun TrayIcons(session: DesktopSession) {
    val app = session.app
    val b = LocalBreeze.current
    val shz by app.shizuku.state.collectAsState()
    val share by app.sharing.stats.collectAsState()
    val notes by PlasmaNotificationService.active.collectAsState()
    val now by rememberNow(30_000)
    val snap by produceState<org.plasma.core.SysSnapshot?>(null, now) {
        value = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) { app.stats.snapshot() }
    }
    val ctl = app.controls
    val ctlVersion by ctl.version.collectAsState()
    val quick by produceState(org.plasma.app.system.QuickState(), ctlVersion, now) {
        value = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) { ctl.quick() }
    }
    fun toggleTray(t: TrayPopup) { val o = session.tray != t; session.closePopups(); if (o) session.tray = t }
    TrayItem(onClick = { session.openTool("settings", "android") }) {
        Canvas(Modifier.size(12.dp)) {
            drawCircle(if (shz == ShizukuState.CONNECTED) Color(0xFF27AE60) else b.dim, size.width / 2)
        }
    }
    if (share.running) TrayItem(onClick = { session.openTool("settings", "sharing") }) {
        Canvas(Modifier.size(12.dp)) { drawCircle(Color(0xFFDA4453), size.width / 2) }
    }
    TrayItem(onClick = { toggleTray(TrayPopup.BRIGHTNESS) }) { BreezeIcon("brightness", 18.dp, b.text) }
    TrayItem(onClick = { toggleTray(TrayPopup.VOLUME) }) {
        BreezeIcon(if (quick.muted || quick.volumePct == 0) "volumeMute" else "volume", 18.dp, b.text)
    }
    TrayItem(onClick = { toggleTray(TrayPopup.BLUETOOTH) }) {
        BreezeIcon("bluetooth", 18.dp, if (quick.btOn) b.text else b.dim.copy(alpha = .55f))
    }
    TrayItem(onClick = { toggleTray(TrayPopup.NETWORK) }) {
        BreezeIcon(if (quick.wifiOn) "wifi" else "wifiOff", 18.dp, if (quick.wifiOn) b.text else b.dim.copy(alpha = .55f))
    }
    TrayItem(onClick = { val o = session.tray != TrayPopup.NOTIFICATIONS; session.closePopups(); if (o) session.tray = TrayPopup.NOTIFICATIONS }) {
        Box {
            Canvas(Modifier.size(18.dp)) {
                val w = size.width
                val p = Path().apply {
                    moveTo(w * .2f, w * .75f); lineTo(w * .2f, w * .45f)
                    cubicTo(w * .2f, w * .15f, w * .8f, w * .15f, w * .8f, w * .45f); lineTo(w * .8f, w * .75f); close()
                }
                drawPath(p, b.text, style = Stroke(1.6.dp.toPx()))
                drawLine(b.text, Offset(w * .1f, w * .78f), Offset(w * .9f, w * .78f), 1.6.dp.toPx())
                drawCircle(b.text, w * .08f, Offset(w * .5f, w * .92f))
            }
            if (notes.isNotEmpty()) T("${notes.size}", 9.sp, Color.White, FontWeight.Bold,
                Modifier.align(Alignment.TopEnd).clip(RoundedCornerShape(6.dp)).background(b.accent).padding(horizontal = 3.dp))
        }
    }
    TrayItem(onClick = { val o = session.tray != TrayPopup.SYSTEM; session.closePopups(); if (o) session.tray = TrayPopup.SYSTEM }) {
        val pct = snap?.batteryPct ?: -1
        Row(verticalAlignment = Alignment.CenterVertically) {
            Canvas(Modifier.size(width = 20.dp, height = 11.dp)) {
                drawRoundRect(b.text, Offset.Zero, Size(size.width - 3.dp.toPx(), size.height), CornerRadius(2.dp.toPx()), style = Stroke(1.2.dp.toPx()))
                drawRect(b.text, Offset(2.dp.toPx(), 2.dp.toPx()), Size((size.width - 7.dp.toPx()) * (pct.coerceIn(0, 100) / 100f), size.height - 4.dp.toPx()))
                drawRect(b.text, Offset(size.width - 2.dp.toPx(), size.height * .3f), Size(2.dp.toPx(), size.height * .4f))
            }
            if (pct >= 0) T("$pct%", 11.sp, b.text, modifier = Modifier.padding(start = 4.dp))
        }
    }
}

@Composable
private fun TrayItem(onClick: () -> Unit, content: @Composable () -> Unit) {
    val tall = LocalTrayTall.current
    Box((if (tall) Modifier.fillMaxHeight() else Modifier.height(40.dp)).padding(vertical = 4.dp).breezeClickable(6.dp) { onClick() }.padding(horizontal = if (tall) 7.dp else 10.dp), contentAlignment = Alignment.Center) { content() }
}

@Composable
private fun ClockButton(session: DesktopSession) {
    val now by rememberNow(60_000)
    val z = Instant.ofEpochMilli(now).atZone(ZoneId.systemDefault())
    val b = LocalBreeze.current
    Column(Modifier.fillMaxHeight().padding(vertical = 4.dp).breezeClickable(6.dp) { val o = session.tray != TrayPopup.CALENDAR; session.closePopups(); if (o) session.tray = TrayPopup.CALENDAR }
        .padding(horizontal = 10.dp), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
        T(DateTimeFormatter.ofPattern("HH:mm").format(z), 14.sp, b.text, FontWeight.Medium)
        T(DateTimeFormatter.ofPattern("dd/MM/yyyy").format(z), 10.sp, b.dim)
    }
}
