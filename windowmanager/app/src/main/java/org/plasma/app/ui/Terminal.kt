
package org.plasma.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.isCtrlPressed
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.buildAnnotatedString
import org.plasma.app.session.Ansi
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.MenuItem
import org.plasma.app.session.MenuState
import org.plasma.app.session.TermSchemes
import org.plasma.app.session.TerminalSession
import org.plasma.app.session.WindowContent
import org.plasma.core.Keys

val LocalWinCorner = compositionLocalOf { 0.dp }

@Composable
fun KonsoleWindow(session: DesktopSession, c: WindowContent.Terminal) {
    val b = LocalBreeze.current
    val st = session.app.settings
    st.watch()
    val scheme = TermSchemes.byId(st.string(Keys.TERM_SCHEME, "breeze"))
    val opacity = st.float(Keys.TERM_OPACITY, 0.82f)
    val fontSp = st.float(Keys.TERM_FONT, 12f)
    val active = c.tabs.getOrNull(c.active) ?: return
    val corner = LocalWinCorner.current
    val winAlpha = LocalWinAlpha.current

    Column(Modifier.fillMaxSize()) {
        MenuBar(session, c, scheme.id, opacity, fontSp)
        TabBar(session, c)
        Box(
            Modifier.weight(1f).fillMaxWidth()
                .glass(scheme.bg, radius = corner, z = 5f, alpha = winAlpha, fixedAlpha = opacity),
        ) {
            TerminalView(active, scheme, fontSp, Modifier.fillMaxSize(), winAlpha)
        }
    }
}

@Composable
private fun MenuBar(session: DesktopSession, c: WindowContent.Terminal, schemeId: String, opacity: Float, fontSp: Float) {
    val b = LocalBreeze.current
    val st = session.app.settings
    fun menu(origin: Offset, items: List<MenuItem>) { session.menu = MenuState(origin.x, origin.y, items) }
    fun newTab() { c.tabs += TerminalSession(session.app, session); c.active = c.tabs.size - 1 }
    val entries: List<Pair<String, () -> List<MenuItem>>> = listOf(
        "Arquivo" to { listOf(
            MenuItem("Nova aba") { newTab() },
            MenuItem("Nova janela") { session.openTool("terminal") },
            MenuItem("", separator = true),
            MenuItem("Fechar aba") { c.tabs.getOrNull(c.active)?.let { session.closeWindowOf(it) } },
        ) },
        "Editar" to { listOf(
            MenuItem("Limpar tela") { c.tabs.getOrNull(c.active)?.clear() },
            MenuItem("Executar fastfetch") { c.tabs.getOrNull(c.active)?.apply { input = "fastfetch"; submit() } },
        ) },
        "Exibir" to { listOf(
            MenuItem("Aumentar fonte") { st.put(Keys.TERM_FONT, (fontSp + 1f).coerceAtMost(28f)) },
            MenuItem("Diminuir fonte") { st.put(Keys.TERM_FONT, (fontSp - 1f).coerceAtLeast(8f)) },
            MenuItem("Tamanho padrão da fonte") { st.put(Keys.TERM_FONT, 12f) },
        ) },
        "Configurações" to { buildList {
            add(MenuItem("Esquema de cores", enabled = false))
            TermSchemes.ALL.forEach { s -> add(MenuItem(s.label, icon = if (s.id == schemeId) "radioOn" else "radioOff") { st.put(Keys.TERM_SCHEME, s.id) }) }
            add(MenuItem("", separator = true))
            add(MenuItem("Transparência do fundo", enabled = false))
            listOf(1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.45f).forEach { o ->
                add(MenuItem("${(100 - o * 100).toInt()}%", icon = if (kotlin.math.abs(o - opacity) < 0.03f) "radioOn" else "radioOff") { st.put(Keys.TERM_OPACITY, o) })
            }
            add(MenuItem("", separator = true))
            add(MenuItem("fastfetch ao abrir", icon = if (st.bool(Keys.TERM_FASTFETCH, true)) "checkOn" else "checkOff") { st.put(Keys.TERM_FASTFETCH, !st.bool(Keys.TERM_FASTFETCH, true)) })
        } },
        "Ajuda" to { listOf(MenuItem("Comandos disponíveis") { c.tabs.getOrNull(c.active)?.apply { input = "help"; submit() } }) },
    )
    Row(Modifier.fillMaxWidth().height(26.dp).background(b.window), verticalAlignment = Alignment.CenterVertically) {
        entries.forEach { (label, items) ->
            var origin by remember { mutableStateOf(Offset.Zero) }
            var h by remember { mutableStateOf(0f) }
            Box(
                Modifier.padding(horizontal = 2.dp).onGloballyPositioned { origin = it.positionInRoot(); h = it.size.height.toFloat() }
                    .breezeClickable(4.dp) { menu(Offset(origin.x, origin.y + h), items()) }
                    .padding(horizontal = 9.dp, vertical = 3.dp),
            ) { T(label, 12.sp, b.text) }
        }
    }
}

@Composable
private fun TabBar(session: DesktopSession, c: WindowContent.Terminal) {
    val b = LocalBreeze.current
    Row(Modifier.fillMaxWidth().height(30.dp).background(b.window), verticalAlignment = Alignment.Bottom) {
        c.tabs.forEachIndexed { i, t ->
            val on = i == c.active
            Row(
                Modifier.height(28.dp).width(150.dp).background(if (on) b.view else Color.Transparent, RoundedCornerShape(topStart = 5.dp, topEnd = 5.dp))
                    .clickable(indication = null, interactionSource = remember { MutableInteractionSource() }) { c.active = i }
                    .padding(start = 10.dp, end = 4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (on) Box(Modifier.size(6.dp).background(b.accent, RoundedCornerShape(3.dp)))
                T(t.tabTitle, 12.sp, if (on) b.text else b.dim, maxLines = 1, modifier = Modifier.weight(1f).padding(start = if (on) 6.dp else 0.dp))
                Box(Modifier.size(20.dp).breezeClickable(10.dp) { session.closeWindowOf(t) }, contentAlignment = Alignment.Center) { BreezeIcon("close", 10.dp, b.dim) }
            }
        }
        Box(Modifier.padding(start = 4.dp, bottom = 2.dp).size(24.dp).breezeClickable(5.dp) {
            c.tabs += TerminalSession(session.app, session); c.active = c.tabs.size - 1
        }, contentAlignment = Alignment.Center) { BreezeIcon("plus", 14.dp, b.dim) }
    }
}

@Composable
private fun TerminalView(t: TerminalSession, scheme: org.plasma.app.session.TermScheme, fontSp: Float, modifier: Modifier, winAlpha: () -> Float) {
    val st = t.lines
    val listState = rememberLazyListState()
    val focus = remember { FocusRequester() }
    val mono = TextStyle(fontFamily = FontFamily.Monospace, fontSize = fontSp.sp, lineHeight = (fontSp * 1.28f).sp, color = scheme.fg)

    var touching by remember { mutableStateOf(false) }
    LaunchedEffect(t.revision, t.busy, touching) { if (!touching) listState.scrollToItem(st.size) }
    LaunchedEffect(t) { runCatching { focus.requestFocus() } }

    Box(modifier.clickable(indication = null, interactionSource = remember { MutableInteractionSource() }) { runCatching { focus.requestFocus() } }) {
        SelectionContainer(Modifier.pointerInput(Unit) {
            awaitEachGesture {
                awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
                touching = true
                do { val e = awaitPointerEvent(PointerEventPass.Final) } while (e.changes.any { it.pressed })
                touching = false
            }
        }) {
            LazyColumn(Modifier.fillMaxSize().padding(horizontal = 8.dp, vertical = 4.dp), state = listState) {
                items(st.size, key = { it }) { i ->
                    val line = st[i]
                    val text = remember(line, scheme.id) { Ansi.render(line.ifEmpty { " " }, scheme) }
                    androidx.compose.material3.Text(text, style = mono, softWrap = false)
                }
                item {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (!t.busy) androidx.compose.material3.Text(Ansi.render(t.promptAnsi(), scheme), style = mono, softWrap = false)
                        else if (t.partial.isNotEmpty()) {
                            val pt = t.partial
                            androidx.compose.material3.Text(remember(pt, scheme.id) { Ansi.render(pt, scheme) }, style = mono, softWrap = false)
                        }
                        BasicTextField(
                            value = t.input, onValueChange = { t.input = it.replace("\n", "") },
                            textStyle = mono, cursorBrush = SolidColor(scheme.cursor), singleLine = true,
                            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                            keyboardActions = KeyboardActions(onSend = { t.submit() }),
                            modifier = Modifier.weight(1f).focusRequester(focus).onPreviewKeyEvent { e ->
                                if (e.type != KeyEventType.KeyDown) return@onPreviewKeyEvent false
                                when {
                                    e.key == Key.Enter || e.key == Key.NumPadEnter -> { t.submit(); true }
                                    e.key == Key.DirectionUp -> { t.historyPrev(); true }
                                    e.key == Key.DirectionDown -> { t.historyNext(); true }
                                    e.key == Key.Tab -> { t.complete(); true }
                                    e.isCtrlPressed && e.key == Key.C && t.input.isEmpty() -> { t.interrupt(); true }
                                    e.isCtrlPressed && e.key == Key.D && t.busy && t.input.isEmpty() -> { t.eof(); true }
                                    e.isCtrlPressed && e.key == Key.C && t.busy -> { t.interrupt(); true }
                                    e.isCtrlPressed && e.key == Key.L -> { t.clear(); true }
                                    else -> false
                                }
                            },
                        )
                    }
                }
            }
        }
    }
}
