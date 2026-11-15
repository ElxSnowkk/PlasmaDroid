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

package org.plasma.app.ui

import android.graphics.SurfaceTexture
import android.os.SystemClock
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.TextureView
import org.plasma.app.android.InputPump
import androidx.compose.foundation.*
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.snap
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.*
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.MenuItem
import org.plasma.app.session.MenuState
import org.plasma.app.session.WindowContent
import kotlinx.coroutines.delay
import org.plasma.bridge.ProjectionHolder
import org.plasma.core.Keys
import org.plasma.wm.Edge
import org.plasma.wm.Frame
import org.plasma.wm.ManagedWindow
import org.plasma.wm.WinState
import org.plasma.wm.WindowAspect
import kotlin.math.roundToInt

@Composable
fun WindowFrame(session: DesktopSession, win: ManagedWindow, focused: Boolean, visible: Boolean) {
    val b = LocalBreeze.current
    val fx = LocalFx.current
    val d = LocalDensity.current
    val maxed = win.state == WinState.MAXIMIZED || win.state == WinState.FULLSCREEN
    val content = session.contents[win.id]
    val translucent = content is WindowContent.Terminal
    val isAndroidWin = content is WindowContent.AndroidWin
    val isClosing = session.closing[win.id] == true
    val shape = RoundedCornerShape(if (maxed) 0.dp else U.frameRadius)
    val frameShape = shape
    val wantShadow = fx.shadows && !maxed && content !is WindowContent.AndroidWin

    val p = remember { Animatable(0f) }
    LaunchedEffect(visible, isClosing, fx.animSpeed) {
        val target = if (visible && !isClosing) 1f else 0f
        session.glass.kick(fx.ms(Motion.LONG + 120))
        if (fx.animSpeed <= 0f) p.snapTo(target)
        else p.animateTo(target, Motion.spec(fx, if (target > 0f) Motion.LONG + 40 else Motion.LONG, if (target > 0f) Motion.OutCubic else Motion.InCubic))
        if (isClosing) session.finalizeClose(win.id)
    }
    val hidden = !visible && p.value <= 0.001f

    val geo = if (session.interacting || content is WindowContent.AndroidWin || fx.animSpeed <= 0f) snap<Float>() else Motion.spec<Float>(fx, Motion.LONG + 30, Motion.InOutCubic)
    val ax by animateFloatAsState(win.frame.x, geo, label = "x")
    val ay by animateFloatAsState(win.frame.y, geo, label = "y")
    val aw by animateFloatAsState(win.frame.w, geo, label = "w")
    val ah by animateFloatAsState(win.frame.h, geo, label = "h")
    LaunchedEffect(win.frame, session.interacting) { if (!session.interacting) session.glass.kick(fx.ms(Motion.LONG + 80)) }

    val sizeMod = if (!hidden) Modifier.size(with(d) { aw.toDp() }, with(d) { ah.toDp() }) else Modifier.size(0.dp)
    CompositionLocalProvider(LocalWinAlpha provides { p.value }, LocalWinCorner provides (if (maxed) 0.dp else U.frameRadius)) {
        Box(
            Modifier.offset { IntOffset(ax.roundToInt(), ay.roundToInt()) }.then(sizeMod)
                .graphicsLayer {
                    val e = p.value
                    alpha = e

                    val sc = if (isAndroidWin) 1f else 0.9f + 0.1f * e
                    scaleX = sc; scaleY = sc
                    transformOrigin = TransformOrigin(0.5f, if (visible) 0.5f else 1f)
                    translationY = if (isAndroidWin) 0f else (1f - e) * (if (visible) 0f else 36f)

                    shadowElevation = if (wantShadow && !session.interacting) (if (focused) 14f else 4f) * density else 0f
                    this.shape = frameShape
                    ambientShadowColor = Color(0x66000000)
                    spotShadowColor = Color(0xAA000000)
                }
                .clip(shape).then(if (translucent) Modifier else Modifier.background(b.window)).border(1.dp, if (focused) b.line else b.line.copy(alpha = .6f), shape)
                .pointerInput(win.id) {
                    awaitPointerEventScope {
                        while (true) {
                            val e = awaitPointerEvent(PointerEventPass.Initial)
                            if (e.type == PointerEventType.Press) {
                                if (session.wm.state.value.focusedId != win.id) session.wm.focus(win.id)
                                if (session.startMenuOpen || session.tray != org.plasma.app.session.TrayPopup.NONE) session.closePopups()
                            }
                        }
                    }
                },
        ) {
            Column(Modifier.fillMaxSize()) {
                if (!hidden && win.state != WinState.FULLSCREEN) TitleBar(session, win, focused)
                Box(Modifier.weight(1f).fillMaxWidth().then(if (translucent) Modifier else Modifier.background(b.view))) {
                    when (val c = content) {
                        is WindowContent.AndroidWin -> AspectFit(session, win) { AndroidWindowContent(session, c) }
                        WindowContent.Files -> FileManager(session)
                        WindowContent.Settings -> SettingsApp(session)
                        WindowContent.Setup -> SetupWindow(session)
                        WindowContent.WaylandDisplay -> WaylandDisplayContent(session)
                        is WindowContent.WaylandClientWindow -> AspectFit(session, win) { WaylandClientWindowContent(session, c.waylandId) }
                        is WindowContent.Terminal -> KonsoleWindow(session, c)
                        is WindowContent.Capture -> CaptureContent(session, c)
                        null -> {}
                    }
                }
            }
            if (!hidden && win.state == WinState.FULLSCREEN) FullscreenOverlay(session, win)
            if (!hidden && win.state == WinState.NORMAL) ResizeHandles(session, win)
        }
    }
}

@Composable
private fun TitleBar(session: DesktopSession, win: ManagedWindow, focused: Boolean) {
    val b = LocalBreeze.current
    val fx = LocalFx.current
    var abs by remember { mutableStateOf(Offset.Zero) }
    val bg by animateColorAsState(if (focused) b.titleOn else b.titleOff, Motion.spec(fx, Motion.LONG), label = "title")
    Row(
        Modifier.fillMaxWidth().height(32.dp).background(bg)
            .pointerInput(win.id) { detectTapGestures(onDoubleTap = { session.wm.toggleMaximize(win.id) }) }
            .pointerInput(win.id) {
                detectDragGestures(
                    onDragStart = { session.interacting = true },
                    onDragEnd = { session.interacting = false; session.wm.dragEnd(win.id, abs.x, abs.y) },
                    onDragCancel = { session.interacting = false },
                ) { c, drag ->
                    c.consume()
                    val f = session.wm.state.value.window(win.id)?.frame ?: return@detectDragGestures
                    abs = Offset(f.x + c.position.x, f.y + c.position.y)
                    session.wm.move(win.id, drag.x, drag.y, abs.x)
                }
            }
            .onSecondaryClick { p -> windowMenu(session, win, p) },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Spacer(Modifier.width(8.dp))
        EntryIcon(win.appId, 16.dp)
        T(win.title, 13.sp, if (focused) b.titleTextOn else b.titleTextOff, if (focused) FontWeight.Medium else null,
            Modifier.weight(1f).padding(horizontal = 8.dp), maxLines = 1)
        if (session.isAppSurface(win.id)) {
            TitleIconButton("rotate") { session.rotateWindow(win.id) }
            TitleIconButton("fullscreen") { session.toggleFullscreen(win.id) }
        }
        TitleButton(0) { session.wm.minimize(win.id) }
        TitleButton(1) { session.wm.toggleMaximize(win.id) }
        TitleButton(2, danger = true) { session.closeWindow(win.id) }
        Spacer(Modifier.width(6.dp))
    }
}

private fun windowMenu(session: DesktopSession, win: ManagedWindow, p: Offset) {
    val count = session.wm.state.value.desktopCount
    session.menu = MenuState(p.x, p.y, buildList {
        add(MenuItem(if (win.state == WinState.MAXIMIZED) "Restaurar" else "Maximizar") { session.wm.toggleMaximize(win.id) })
        add(MenuItem(if (win.state == WinState.FULLSCREEN) "Sair da tela cheia" else "Tela cheia") { session.toggleFullscreen(win.id) })
        if (session.isAppSurface(win.id)) add(MenuItem("Girar janela") { session.rotateWindow(win.id) })
        add(MenuItem("Minimizar") { session.wm.minimize(win.id) })
        add(MenuItem("Ancorar à esquerda") { session.wm.snap(win.id, true) })
        add(MenuItem("Ancorar à direita") { session.wm.snap(win.id, false) })
        add(MenuItem("", separator = true))
        for (n in 0 until count) add(MenuItem("Mover para a área de trabalho ${n + 1}", n != win.desktop) { session.wm.moveToDesktop(win.id, n) })
        add(MenuItem("", separator = true))
        add(MenuItem("Fechar") { session.closeWindow(win.id) })
    })
}

@Composable
private fun TitleButton(kind: Int, danger: Boolean = false, onClick: () -> Unit) {
    val b = LocalBreeze.current
    var hover by remember { mutableStateOf(false) }
    Box(
        Modifier.size(24.dp).clip(CircleShape).background(if (hover) (if (danger) Color(0xFFDA4453) else b.hover) else Color.Transparent)
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) { val e = awaitPointerEvent(); hover = e.type == PointerEventType.Enter || e.type == PointerEventType.Move; if (e.type == PointerEventType.Exit) hover = false }
                }
            }.clickable { onClick() },
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.size(10.dp)) {
            val w = size.width; val sw = 1.5.dp.toPx(); val c = if (hover && danger) Color.White else b.text
            when (kind) {
                0 -> drawLine(c, Offset(0f, w * .8f), Offset(w, w * .8f), sw)
                1 -> drawRect(c, style = Stroke(sw))
                else -> { drawLine(c, Offset(0f, 0f), Offset(w, w), sw); drawLine(c, Offset(w, 0f), Offset(0f, w), sw) }
            }
        }
    }
}

@Composable
private fun TitleIconButton(icon: String, onClick: () -> Unit) {
    val b = LocalBreeze.current
    var hover by remember { mutableStateOf(false) }
    Box(
        Modifier.size(24.dp).clip(CircleShape).background(if (hover) b.hover else Color.Transparent)
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) { val e = awaitPointerEvent(); hover = e.type == PointerEventType.Enter || e.type == PointerEventType.Move; if (e.type == PointerEventType.Exit) hover = false }
                }
            }.clickable { onClick() },
        contentAlignment = Alignment.Center,
    ) { BreezeIcon(icon, 13.dp, b.text) }
}

@Composable
private fun BoxScope.FullscreenOverlay(session: DesktopSession, win: ManagedWindow) {
    var open by remember { mutableStateOf(false) }
    var touched by remember { mutableIntStateOf(0) }
    LaunchedEffect(open, touched) { if (open) { delay(3500); open = false } }
    val canRotate = session.isAppSurface(win.id)
    Box(
        Modifier.align(Alignment.TopCenter).width(150.dp).height(40.dp)
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) {
                        val e = awaitPointerEvent()
                        if (e.type == PointerEventType.Enter || e.type == PointerEventType.Move) { open = true; touched++ }
                    }
                }
            }
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { open = !open; touched++ },
        contentAlignment = Alignment.TopCenter,
    ) {
        if (open) {
            Row(
                Modifier.padding(top = 6.dp).clip(RoundedCornerShape(18.dp)).background(Color(0xCC1B1E20)).padding(horizontal = 6.dp, vertical = 3.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (canRotate) FullscreenPillButton("rotate") { session.rotateWindow(win.id); touched++ }
                FullscreenPillButton("fullscreenExit") { session.toggleFullscreen(win.id) }
            }
        } else {
            Box(Modifier.padding(top = 6.dp).width(44.dp).height(4.dp).clip(CircleShape).background(Color(0x66FFFFFF)))
        }
    }
}

@Composable
private fun FullscreenPillButton(icon: String, onClick: () -> Unit) {
    Box(Modifier.size(32.dp).clip(CircleShape).clickable { onClick() }, contentAlignment = Alignment.Center) {
        BreezeIcon(icon, 16.dp, Color.White)
    }
}

@Composable
private fun AspectFit(session: DesktopSession, win: ManagedWindow, content: @Composable () -> Unit) {
    val st = session.app.settings
    st.watch()
    val d = LocalDensity.current
    val force = win.state == WinState.FULLSCREEN && st.bool(Keys.FORCE_ASPECT, true)
    BoxWithConstraints(Modifier.fillMaxSize().background(Color.Black)) {
        val area = Frame(0f, 0f, constraints.maxWidth.toFloat(), constraints.maxHeight.toFloat())
        val fitted = if (force && area.w > 0f && area.h > 0f) {
            val ratio = WindowAspect.targetRatio(
                st.string(Keys.ASPECT_RATIO, WindowAspect.DEFAULT),
                session.wm.state.value.screenArea,
                session.effectiveOrientation(win.id),
            )
            WindowAspect.fit(area, ratio)
        } else null
        val inner = if (fitted != null) {
            Modifier.align(Alignment.Center).size(with(d) { fitted.w.toDp() }, with(d) { fitted.h.toDp() })
        } else Modifier.fillMaxSize()
        Box(inner) { content() }
    }
}

@Composable
private fun BoxScope.ResizeHandles(session: DesktopSession, win: ManagedWindow) {
    @Composable fun handle(edges: Int, mod: Modifier) {
        Box(mod.pointerInput(win.id, edges) {
            detectDragGestures(onDragStart = { session.interacting = true }, onDragEnd = { session.interacting = false }, onDragCancel = { session.interacting = false }) { c, d -> c.consume(); session.wm.resize(win.id, edges, d.x, d.y) }
        })
    }
    val t = 6.dp; val k = 14.dp
    handle(Edge.LEFT, Modifier.align(Alignment.CenterStart).fillMaxHeight().width(t))
    handle(Edge.RIGHT, Modifier.align(Alignment.CenterEnd).fillMaxHeight().width(t))
    handle(Edge.TOP, Modifier.align(Alignment.TopCenter).fillMaxWidth().height(t))
    handle(Edge.BOTTOM, Modifier.align(Alignment.BottomCenter).fillMaxWidth().height(t))
    handle(Edge.LEFT or Edge.TOP, Modifier.align(Alignment.TopStart).size(k))
    handle(Edge.RIGHT or Edge.TOP, Modifier.align(Alignment.TopEnd).size(k))
    handle(Edge.LEFT or Edge.BOTTOM, Modifier.align(Alignment.BottomStart).size(k))
    handle(Edge.RIGHT or Edge.BOTTOM, Modifier.align(Alignment.BottomEnd).size(k))
}

@Composable
private fun AndroidWindowContent(session: DesktopSession, c: WindowContent.AndroidWin) {
    val b = LocalBreeze.current
    Box(Modifier.fillMaxSize().background(Color.Black)) {
        AndroidView(
            factory = { ctx ->
                TextureView(ctx).apply {
                    isOpaque = true
                    surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                        override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) { if (w > 0 && h > 0) session.launcher.onSurface(c, st, w, h) }
                        override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) { session.launcher.onResize(c, w, h) }
                        override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean { session.launcher.release(c); return true }
                        override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
                    }
                }
            },
            modifier = Modifier.fillMaxSize(),
        )

        Box(Modifier.fillMaxSize().androidInput(c) { session.app.backends.current() })
    }
}

@Composable
private fun CaptureContent(session: DesktopSession, c: WindowContent.Capture) {
    val ctx = LocalContext.current
    val active by ProjectionHolder.active.collectAsState()
    var tex by remember { mutableStateOf<SurfaceTexture?>(null) }
    var dims by remember { mutableStateOf(0 to 0) }
    LaunchedEffect(active, tex, dims) {
        val st = tex ?: return@LaunchedEffect
        if (active && c.vd == null && dims.first > 0) {
            st.setDefaultBufferSize(dims.first, dims.second)
            val s = android.view.Surface(st); c.surface = s
            c.vd = ProjectionHolder.mirror(ctx, s, dims.first, dims.second, ctx.resources.displayMetrics.densityDpi)
            c.status = if (c.vd != null) "Espelhando a tela principal" else "Falha ao criar a captura"
        }
        if (!active) { c.vd?.release(); c.vd = null; c.status = "Aguardando autorização…" }
    }
    Box(Modifier.fillMaxSize().background(Color.Black)) {
        AndroidView(factory = { x ->
            TextureView(x).apply {
                surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                    override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) { tex = st; dims = w to h }
                    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {}
                    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean { c.vd?.release(); c.vd = null; tex = null; return true }
                    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
                }
            }
        }, modifier = Modifier.fillMaxSize())
        Column(Modifier.align(Alignment.BottomStart).padding(10.dp).clip(RoundedCornerShape(6.dp)).background(Color(0xAA000000)).padding(10.dp)) {
            T(c.status, 12.sp, Color.White)
            T("Mostra a tela principal do aparelho (útil em monitor externo ou ao compartilhar).", 11.sp, Color(0xFFBBBBBB))
            if (!active) BreezeButton("Autorizar captura de tela", { session.requestProjection() }, Modifier.padding(top = 8.dp), primary = true)
        }
    }
}
