
package org.plasma.app.ui

import androidx.compose.foundation.*
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.animation.core.Animatable
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.graphics.graphicsLayer
import org.plasma.render.gl.FxConfig
import org.plasma.render.gl.Wallpapers
import org.plasma.wm.WinState
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.zIndex
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.withContext
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.MenuItem
import org.plasma.app.session.MenuState
import org.plasma.app.session.TrayPopup
import org.plasma.core.Keys
import org.plasma.wm.WmState
import kotlin.math.roundToInt

@Composable
fun DesktopRoot(session: DesktopSession) {
    val st = session.app.settings
    st.watch()
    val wm by session.wm.state.collectAsState()
    val density = LocalDensity.current
    val fx = LocalFx.current
    val panelDp = st.int(Keys.PANEL_H, U.DEFAULT_PANEL_DP)
    val panelTop = st.bool(Keys.PANEL_TOP, false)
    val floating = st.bool(Keys.PANEL_FLOATING, true)
    val adaptive = st.bool(Keys.PANEL_ADAPTIVE, true)
    val marginDp = if (floating) st.int(Keys.PANEL_MARGIN, U.DEFAULT_PANEL_MARGIN_DP).coerceIn(0, 48) else 0
    val totalDp = panelDp + marginDp * 2
    val panelPx = with(density) { totalDp.dp.toPx() }
    var size by remember { mutableStateOf(IntSize.Zero) }
    LaunchedEffect(size, panelPx, panelTop) {
        if (size != IntSize.Zero) session.setScreen(size.width.toFloat(), size.height.toFloat(), panelPx, panelTop)
    }
    LaunchedEffect(st.int(Keys.DESKTOPS, 2)) { session.wm.setDesktopCount(st.int(Keys.DESKTOPS, 2)) }
    WaylandDisplayContent(session, size)
    LaunchedEffect(session.toast) { if (session.toast != null) { delay(3000); session.toast = null } }

    val maxed = !wm.showingDesktop && wm.windows.any { it.desktop == wm.desktop && !it.minimized && (it.state == WinState.MAXIMIZED || it.state == WinState.FULLSCREEN) }
    val fullscreen = !wm.showingDesktop && wm.windows.any { it.desktop == wm.desktop && !it.minimized && it.state == WinState.FULLSCREEN }
    val opaque = adaptive && (maxed || fullscreen)
    val wall = st.string(Keys.WALLPAPER, "theme")
    val theme = Themes.byId(st.string(Keys.THEME, "breeze-dark"))
    val wallId = if (wall == "theme" || wall.isEmpty()) theme.wallpaper else wall.removePrefix("preset:")
    val ctx = LocalContext.current
    val hasAndroidApp = wm.windows.any {
        it.desktop == wm.desktop && !it.minimized && session.contents[it.id] is org.plasma.app.session.WindowContent.AndroidWin
    }
    val fxCfg = FxConfig(
        blur = fx.blur, blurStrength = st.float(Keys.FX_BLUR_STRENGTH, 0.6f), saturation = st.float(Keys.FX_SATURATION, 1.25f),
        wallpaper = Wallpapers.byId(wallId), animated = st.bool(Keys.FX_WALL_ANIM, true) && !maxed && !hasAndroidApp,

        fpsCap = st.int(Keys.FX_FPS, 60).coerceIn(12, 60).let { if (hasAndroidApp) minOf(it, 30) else it },
        vignette = st.float(Keys.FX_VIGNETTE, 0.35f),
    )
    SideEffect { session.compositor.setFx(fxCfg) }
    val image = wall.startsWith("content:") || wall.startsWith("file:")
    val bmp by produceState<androidx.compose.ui.graphics.ImageBitmap?>(null, wall) {
        value = if (image) withContext(Dispatchers.IO) { loadBitmap(ctx, wall, 2048) } else null
    }
    LaunchedEffect(bmp) { session.compositor.setWallpaperBitmap(bmp?.asAndroidBitmap()) }
    GlassPump(session.glass, session.compositor, fxCfg.saturation)

    CompositionLocalProvider(LocalSession provides session, LocalGlass provides session.glass) {
        Box(Modifier.fillMaxSize().background(Color(Themes.byId(theme.id).view)).onSizeChanged { size = it }) {
            GlSurface(session.compositor)
            DesktopSurface(session, totalDp, panelTop)
            WindowsLayer(session, wm)
            val align = if (panelTop) Alignment.TopCenter else Alignment.BottomCenter
            if (!fullscreen) Panel(session, wm, panelDp, floating, opaque, Modifier.align(align).zIndex(1000f))
            PopupLayer(session, totalDp, panelTop)
            WidgetPicker(session)
            ContextMenuHost(session)
            session.toast?.let { t -> Toast(t, Modifier.align(Alignment.TopCenter).padding(top = 48.dp).zIndex(3000f)) }
            if (session.locked) LockScreen(session)
        }
    }
}

@Composable
private fun Toast(t: String, modifier: Modifier) {
    val b = LocalBreeze.current
    val fx = LocalFx.current
    val p = remember { Animatable(0f) }
    LaunchedEffect(Unit) { p.animateTo(1f, Motion.spec(fx, Motion.LONG)) }
    Box(modifier.graphicsLayer { alpha = p.value; translationY = (1f - p.value) * -24f }
        .glass(b.window, 8.dp, shadow = 12.dp, z = 30f).border(1.dp, b.line, RoundedCornerShape(8.dp))
        .padding(horizontal = 16.dp, vertical = 10.dp)) { T(t) }
}

@Composable
fun DesktopSurface(session: DesktopSession, panelDp: Int, panelTop: Boolean) {
    val st = session.app.settings
    st.watch()
    val apps by session.app.apps.collectAsState()
    val icons = st.list(Keys.DESK_ICONS, "tool:files,tool:terminal,tool:settings")
    val pad = Modifier.padding(top = if (panelTop) panelDp.dp else 0.dp, bottom = if (panelTop) 0.dp else panelDp.dp)
    Box(
        Modifier.fillMaxSize().then(pad)
            .pointerInput(Unit) { detectTapGestures(onTap = { session.closePopups() }, onLongPress = { p -> desktopMenu(session, p) }) }
            .onSecondaryClick { desktopMenu(session, it) },
    ) {
        Column(Modifier.padding(12.dp)) {
            icons.forEach { id ->
                Column(
                    Modifier.width(88.dp).padding(vertical = 4.dp).clip(RoundedCornerShape(6.dp))
                        .clickable { openById(session, id) }
                        .onSecondaryClick { p ->
                            session.menu = MenuState(p.x, p.y, listOf(
                                MenuItem("Abrir") { openById(session, id) },
                                MenuItem("Remover da área de trabalho") { st.putList(Keys.DESK_ICONS, icons - id) }))
                        }.padding(6.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    EntryIcon(id, U.iconL)
                    T(entryLabel(id, apps), 12.sp, Color.White, maxLines = 2, align = TextAlign.Center)
                }
            }
        }
        DesktopWidgets(session)
    }
}

private fun desktopMenu(session: DesktopSession, p: Offset) {
    session.menu = MenuState(p.x, p.y, listOf(
        MenuItem("Adicionar widgets…") { session.widgetPicker = true },
        MenuItem("Trocar papel de parede…") { session.openTool("settings", "appearance") },
        MenuItem("Configurar a área de trabalho…") { session.openTool("settings", "appearance") },
        MenuItem("", separator = true),
        MenuItem("Abrir Dolphin") { session.openTool("files") },
        MenuItem("Abrir Konsole") { session.openTool("terminal") },
        MenuItem("Configurações do Sistema") { session.openTool("settings") },
        MenuItem("", separator = true),
        MenuItem("Bloquear a tela") { session.locked = true },
    ))
}

@Composable
fun WindowsLayer(session: DesktopSession, wm: WmState) {
    Box(Modifier.fillMaxSize()) {
        wm.windows.forEachIndexed { index, w ->
            key(w.id) {
                val visible = !wm.showingDesktop && w.desktop == wm.desktop && !w.minimized
                Box(Modifier.zIndex(index.toFloat())) {
                    WindowFrame(session, w, focused = wm.focusedId == w.id, visible = visible)
                }
            }
        }
    }
}

private val WIDGET_TYPES = listOf("clock" to "Relógio", "sysmon" to "Monitor do sistema", "calendar" to "Calendário", "notes" to "Notas")

@Composable
fun DesktopWidgets(session: DesktopSession) {
    val st = session.app.settings
    st.watch()
    val items = st.list(Keys.WIDGETS)
    items.forEachIndexed { idx, raw ->
        val p = raw.split("@")
        if (p.size < 3) return@forEachIndexed
        val type = p[0]
        var ox by remember(raw) { mutableFloatStateOf(p[1].toFloatOrNull() ?: 0f) }
        var oy by remember(raw) { mutableFloatStateOf(p[2].toFloatOrNull() ?: 0f) }
        val b = LocalBreeze.current
        Column(
            Modifier.offset { IntOffset(ox.roundToInt(), oy.roundToInt()) }.width(if (type == "sysmon") 260.dp else 230.dp)
                .glass(b.window, 8.dp, shadow = 10.dp, z = 4f).border(1.dp, b.line, RoundedCornerShape(8.dp))
                .onSecondaryClick { pos ->
                    session.menu = MenuState(pos.x, pos.y, listOf(MenuItem("Remover widget") {
                        st.putList(Keys.WIDGETS, st.list(Keys.WIDGETS).toMutableList().also { it.remove(raw) })
                    }))
                },
        ) {
            Box(Modifier.fillMaxWidth().height(22.dp).background(b.hover).pointerInput(raw) {
                detectDragGestures(onDragEnd = {
                    val l = st.list(Keys.WIDGETS).toMutableList()
                    val i = l.indexOf(raw)
                    if (i >= 0) { l[i] = "$type@${ox.roundToInt()}@${oy.roundToInt()}"; st.putList(Keys.WIDGETS, l) }
                }) { c, d -> c.consume(); ox += d.x; oy += d.y }
            }, contentAlignment = Alignment.CenterStart) {
                T(WIDGET_TYPES.firstOrNull { it.first == type }?.second ?: type, 11.sp, b.dim, modifier = Modifier.padding(horizontal = 8.dp))
            }
            Box(Modifier.padding(10.dp)) {
                when (type) {
                    "clock" -> ClockWidget()
                    "sysmon" -> SystemMonitorView(session)
                    "calendar" -> CalendarView()
                    "notes" -> NotesWidget(session)
                }
            }
        }
    }
}

@Composable
fun WidgetPicker(session: DesktopSession) {
    if (!session.widgetPicker) return
    val st = session.app.settings
    val b = LocalBreeze.current
    Box(Modifier.fillMaxSize().zIndex(2500f).background(Color(0x66000000)).pointerInput(Unit) { detectTapGestures { session.widgetPicker = false } },
        contentAlignment = Alignment.Center) {
        PopupSurface(Modifier.width(300.dp).padding(8.dp)) {
            T("Adicionar widgets", 15.sp, weight = FontWeight.SemiBold, modifier = Modifier.padding(12.dp))
            WIDGET_TYPES.forEach { (id, label) ->
                Box(Modifier.fillMaxWidth().clickable {
                    val n = st.list(Keys.WIDGETS).size
                    st.putList(Keys.WIDGETS, st.list(Keys.WIDGETS) + "$id@${360 + n * 24}@${40 + n * 24}")
                    session.widgetPicker = false
                }.padding(horizontal = 14.dp, vertical = 10.dp)) { T(label) }
            }
        }
    }
}
