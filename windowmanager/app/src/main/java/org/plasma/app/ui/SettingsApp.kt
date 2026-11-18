
package org.plasma.app.ui

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import org.plasma.app.session.Ansi
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.Fastfetch
import org.plasma.app.session.TermSchemes
import org.plasma.core.Keys
import org.plasma.render.gl.Wallpapers
import kotlin.math.roundToInt

private class Cat(val id: String, val label: String, val group: String, val keywords: String, val glyph: String)

private val CATS = listOf(
    Cat("highlights", "Destaques", "", "início resumo favoritos", "star"),
    Cat("appearance", "Tema global e papel de parede", "Aparência e Estilo", "tema global cores destaque wallpaper papel de parede accent", "palette"),
    Cat("effects", "Efeitos da área de trabalho", "Aparência e Estilo", "desfoque blur transparência sombras animação velocidade shaders", "sparkle"),
    Cat("panel", "Painel", "Área de Trabalho", "painel flutuante adaptativo altura topo", "panel"),
    Cat("terminal", "Konsole", "Área de Trabalho", "terminal konsole fonte esquema fastfetch transparência", "terminal"),
    Cat("displays", "Tela e monitor", "Hardware", "escala resolução monitor externo hz dpi tela cheia proporção aspecto orientação girar", "monitor"),
    Cat("audio", "Som", "Hardware", "som volume áudio alto-falante mudo saída fones toque", "volume"),
    Cat("brightness", "Brilho e cor", "Hardware", "brilho tela luz noturna automático cor", "brightness"),
    Cat("render", "Compositor e GPU", "Hardware", "gpu opengl vulkan egl renderizador fps driver injetar importar bionic plasmagl plasmavk graphics environment", "chip"),
    Cat("network", "Rede e Wi-Fi", "Rede e Conectividade", "wifi wi-fi internet rede conexão senha ip sinal", "wifi"),
    Cat("bluetooth", "Bluetooth", "Rede e Conectividade", "bluetooth dispositivos pareamento fones conectar", "bluetooth"),
    Cat("android", "Integração Android", "Rede e Conectividade", "shizuku root captura notificações janelas android", "android"),
    Cat("sharing", "Compartilhar desktop", "Rede e Conectividade", "stream h264 pin rede transmissão", "share"),
    Cat("shortcuts", "Atalhos", "Fluxo de Trabalho", "teclado atalhos super alt", "keyboard"),
    Cat("system", "Sistema e logs", "Sistema", "logs diagnóstico desempenho", "list"),
    Cat("about", "Sobre este sistema", "Sistema", "sobre info fastfetch versão dispositivo", "info"),
)

@Composable
fun SettingsApp(session: DesktopSession) {
    val b = LocalBreeze.current
    val fx = LocalFx.current
    session.app.settings.watch()
    var query by remember { mutableStateOf("") }
    val q = query.trim()
    val filtered = remember(q) { if (q.isEmpty()) CATS else CATS.filter { (it.label + " " + it.keywords + " " + it.group).contains(q, ignoreCase = true) } }
    val page = if (CATS.any { it.id == session.settingsPage }) session.settingsPage else "appearance"

    Row(Modifier.fillMaxSize()) {
        Column(Modifier.width(220.dp).fillMaxHeight().background(b.view.copy(alpha = .55f))) {
            Box(Modifier.fillMaxWidth().padding(10.dp).clip(RoundedCornerShape(6.dp)).background(b.window).border(1.dp, b.line, RoundedCornerShape(6.dp)).padding(horizontal = 10.dp, vertical = 8.dp)) {
                if (query.isEmpty()) Row(verticalAlignment = Alignment.CenterVertically) { BreezeIcon("search", 14.dp, b.dim); Spacer(Modifier.width(6.dp)); T("Pesquisar…", 13.sp, b.dim) }
                BasicTextField(query, { query = it }, Modifier.fillMaxWidth(), singleLine = true,
                    textStyle = TextStyle(color = b.text, fontSize = 13.sp), cursorBrush = SolidColor(b.accent))
            }
            Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 6.dp)) {
                var lastGroup = "\u0000"
                filtered.forEach { c ->
                    if (c.group != lastGroup && c.group.isNotEmpty()) {
                        T(c.group, 11.sp, b.dim, FontWeight.SemiBold, Modifier.padding(start = 10.dp, top = 12.dp, bottom = 3.dp))
                    }
                    lastGroup = c.group
                    val on = page == c.id
                    val a by animateFloatAsState(if (on) 1f else 0f, Motion.spec(fx, Motion.SHORT), label = "cat")
                    Row(
                        Modifier.fillMaxWidth().padding(vertical = 1.dp).clip(RoundedCornerShape(6.dp))
                            .drawBehind { drawRect(b.accent.copy(alpha = a)) }
                            .breezeClickable(6.dp) { session.settingsPage = c.id }
                            .padding(horizontal = 10.dp, vertical = 9.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        BreezeIcon(c.glyph, 16.dp, if (on) b.onAccent else b.accent); Spacer(Modifier.width(8.dp))
                        T(c.label, 13.sp, if (on) b.onAccent else b.text, maxLines = 1)
                    }
                }
                if (filtered.isEmpty()) T("Nada encontrado para “$q”", 12.sp, b.dim, modifier = Modifier.padding(12.dp))
            }
        }
        Box(Modifier.width(1.dp).fillMaxHeight().background(b.line.copy(alpha = .7f)))
        Column(Modifier.weight(1f).fillMaxHeight().background(b.window.copy(alpha = .7f))) {
            val cat = CATS.first { it.id == page }
            Row(Modifier.fillMaxWidth().height(48.dp).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
                T(cat.label, 20.sp, weight = FontWeight.Light)
            }
            Box(Modifier.fillMaxWidth().height(1.dp).background(b.line.copy(alpha = .6f)))
            val p = remember(page) { Animatable(0f) }
            LaunchedEffect(page) { p.animateTo(1f, Motion.spec(fx, Motion.LONG)) }
            Column(
                Modifier.weight(1f).fillMaxWidth().graphicsLayer { alpha = p.value; translationY = (1f - p.value) * 14f }
                    .verticalScroll(rememberScrollState()).padding(20.dp),
            ) {
                when (page) {
                    "highlights" -> HighlightsPage(session)
                    "appearance" -> ThemesPage(session)
                    "effects" -> EffectsPage(session)
                    "panel" -> PanelPage(session)
                    "terminal" -> TerminalPage(session)
                    "displays" -> { ScalePage(session); FullscreenPage(session); DisplaysPage(session) }
                    "render" -> { GpuPage(session); GraphicsEnvironmentSection(session, Modifier.padding(top = 6.dp)); RenderPage(session) }
                    "network" -> NetworkPanel(session, inSettings = true)
                    "bluetooth" -> BluetoothPanel(session, inSettings = true)
                    "audio" -> VolumePanel(session, inSettings = true)
                    "brightness" -> BrightnessPanel(session, inSettings = true)
                    "android" -> AndroidPage(session)
                    "sharing" -> SharingPage(session)
                    "shortcuts" -> ShortcutsPage()
                    "system" -> SystemPage(session)
                    "about" -> AboutPage(session)
                }
            }
        }
    }
}

@Composable
private fun Card(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    val b = LocalBreeze.current
    Column(modifier.fillMaxWidth().padding(vertical = 6.dp).clip(RoundedCornerShape(8.dp)).background(b.view.copy(alpha = .6f))
        .border(1.dp, b.line.copy(alpha = .6f), RoundedCornerShape(8.dp)).padding(14.dp), content = content)
}

@Composable
private fun Tile(label: String, glyph: String, onClick: () -> Unit, modifier: Modifier = Modifier) {
    val b = LocalBreeze.current
    Column(modifier.clip(RoundedCornerShape(8.dp)).background(b.view.copy(alpha = .6f)).border(1.dp, b.line.copy(alpha = .6f), RoundedCornerShape(8.dp))
        .breezeClickable(8.dp) { onClick() }.padding(14.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        BreezeIcon(glyph, 28.dp, b.accent)
        T(label, 12.sp, maxLines = 2, align = androidx.compose.ui.text.style.TextAlign.Center, modifier = Modifier.padding(top = 6.dp))
    }
}

@Composable
private fun HighlightsPage(session: DesktopSession) {
    val b = LocalBreeze.current
    val theme = Themes.byId(session.app.settings.string(Keys.THEME, "breeze-dark"))
    Card {
        T("Tema atual: ${theme.label}", 15.sp, weight = FontWeight.SemiBold)
        T("Personalize a aparência, os efeitos e a escala da interface.", 12.sp, b.dim)
    }
    SectionTitle("Mais usados")
    listOf(listOf("appearance", "effects", "displays"), listOf("panel", "terminal", "render"), listOf("network", "bluetooth", "audio")).forEach { row ->
        Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            row.forEach { id -> val c = CATS.first { it.id == id }; Tile(c.label, c.glyph, { session.settingsPage = id }, Modifier.weight(1f)) }
        }
    }
}

@Composable
private fun ThemesPage(session: DesktopSession) {
    val st = session.app.settings; val b = LocalBreeze.current
    val cur = st.string(Keys.THEME, "breeze-dark")
    SectionTitle("Tema global")
    Themes.ALL.chunked(2).forEach { row ->
        Row(Modifier.fillMaxWidth().padding(vertical = 5.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            row.forEach { t ->
                val on = t.id == cur
                val w = Wallpapers.byId(t.wallpaper)
                Column(Modifier.weight(1f).clip(RoundedCornerShape(8.dp)).border(if (on) 2.dp else 1.dp, if (on) b.accent else b.line, RoundedCornerShape(8.dp))
                    .breezeClickable(8.dp) { st.put(Keys.THEME, t.id); st.put(Keys.ACCENT, U.THEME_ACCENT); st.put(Keys.WALLPAPER, "theme") }) {
                    Box(Modifier.fillMaxWidth().height(86.dp).background(Brush.linearGradient(w.palette.map { Color(it) }))) {
                        Box(Modifier.align(Alignment.Center).offset(y = (-4).dp).size(96.dp, 46.dp).clip(RoundedCornerShape(5.dp)).background(Color(t.window))) {
                            Box(Modifier.fillMaxWidth().height(10.dp).background(Color(t.titleOn)))
                            Box(Modifier.align(Alignment.BottomStart).padding(6.dp).size(34.dp, 6.dp).clip(RoundedCornerShape(2.dp)).background(Color(t.accent)))
                        }
                        Box(Modifier.align(Alignment.BottomCenter).padding(bottom = 4.dp).size(120.dp, 10.dp).clip(RoundedCornerShape(3.dp)).background(Color(t.view).copy(alpha = .85f)))
                    }
                    Column(Modifier.background(b.view.copy(alpha = .8f)).padding(10.dp)) {
                        T(t.label, 13.sp, weight = FontWeight.SemiBold, maxLines = 1)
                        T(t.author, 11.sp, b.dim, maxLines = 1)
                    }
                }
            }
            if (row.size == 1) Spacer(Modifier.weight(1f))
        }
    }
    SectionTitle("Cor de destaque")
    Row(Modifier.horizontalScroll(rememberScrollState())) {
        val raw = st.int(Keys.ACCENT, U.THEME_ACCENT)
        Box(Modifier.padding(end = 8.dp).size(32.dp).clip(CircleShape).background(Color(Themes.byId(cur).accent))
            .border(if (raw == U.THEME_ACCENT) 3.dp else 1.dp, if (raw == U.THEME_ACCENT) b.text else b.line, CircleShape)
            .breezeClickable(16.dp) { st.put(Keys.ACCENT, U.THEME_ACCENT) }, contentAlignment = Alignment.Center) { T("A", 12.sp, Color.White) }
        ACCENTS.forEach { c ->
            val argb = c.toInt()
            Box(Modifier.padding(end = 8.dp).size(32.dp).clip(CircleShape).background(Color(argb))
                .border(if (raw == argb) 3.dp else 0.dp, b.text, CircleShape)
                .breezeClickable(16.dp) { st.put(Keys.ACCENT, argb) })
        }
    }
    T("“A” segue a cor do tema.", 11.sp, b.dim, modifier = Modifier.padding(top = 4.dp))
    SectionTitle("Papel de parede")
    val wall = st.string(Keys.WALLPAPER, "theme")
    Row(Modifier.horizontalScroll(rememberScrollState())) {
        (listOf("theme" to "Do tema") + Wallpapers.ALL.map { "preset:${it.id}" to it.label }).forEach { (id, label) ->
            val def = if (id == "theme") Wallpapers.byId(Themes.byId(cur).wallpaper) else Wallpapers.byId(id.removePrefix("preset:"))
            val on = wall == id || (wall.isEmpty() && id == "theme")
            Column(Modifier.padding(end = 10.dp).width(118.dp)) {
                Box(Modifier.size(118.dp, 70.dp).clip(RoundedCornerShape(6.dp)).background(Brush.linearGradient(def.palette.map { Color(it) }))
                    .border(if (on) 3.dp else 1.dp, if (on) b.accent else b.line, RoundedCornerShape(6.dp)).breezeClickable(6.dp) { st.put(Keys.WALLPAPER, id) })
                T(label, 11.sp, modifier = Modifier.padding(top = 3.dp), maxLines = 1)
            }
        }
    }
    Row(Modifier.padding(top = 8.dp)) { BreezeButton("Escolher imagem…", { session.pickWallpaper() }) }
    T("Os papéis de parede predefinidos são shaders GLSL animados, gerados em tempo real na GPU.", 11.sp, b.dim, modifier = Modifier.padding(top = 6.dp))
}

@Composable
private fun EffectsPage(session: DesktopSession) {
    val st = session.app.settings; val b = LocalBreeze.current
    val gpu by session.compositor.info.collectAsState()
    if (!gpu.ready) Card { T("Compositor GPU indisponível${if (gpu.error.isNotEmpty()) ": ${gpu.error}" else ""}. O desfoque real fica desligado; a transparência usa um fallback.", 12.sp, b.negative) }
    SectionTitle("Desfoque e transparência")
    SwitchRow("Desfoque do fundo (Dual Kawase)", st.bool(Keys.FX_BLUR, true), "Desfoca o papel de parede atrás de painéis, menus e janelas translúcidas.") { st.put(Keys.FX_BLUR, it) }
    SliderRow("Intensidade do desfoque", st.float(Keys.FX_BLUR_STRENGTH, 0.6f), 0f..1f, "${(st.float(Keys.FX_BLUR_STRENGTH, 0.6f) * 100).roundToInt()}%") { st.put(Keys.FX_BLUR_STRENGTH, it) }
    SliderRow("Transparência", st.float(Keys.FX_TRANSPARENCY, 0.5f), 0f..1f, "${(st.float(Keys.FX_TRANSPARENCY, 0.5f) * 100).roundToInt()}%") { st.put(Keys.FX_TRANSPARENCY, it) }
    SliderRow("Saturação do fundo desfocado", st.float(Keys.FX_SATURATION, 1.25f), 0.5f..2f, "%.2f".format(st.float(Keys.FX_SATURATION, 1.25f))) { st.put(Keys.FX_SATURATION, it) }
    SwitchRow("Sombras", st.bool(Keys.FX_SHADOWS, true)) { st.put(Keys.FX_SHADOWS, it) }
    SectionTitle("Animações")
    val sp = st.float(Keys.FX_ANIM_SPEED, 1f)
    SliderRow("Duração das animações", sp, 0f..2f, if (sp <= 0.01f) "Instantânea" else "%.1fx".format(sp)) { st.put(Keys.FX_ANIM_SPEED, (it * 10).roundToInt() / 10f) }
    SwitchRow("Papel de parede animado", st.bool(Keys.FX_WALL_ANIM, true), "Desligue para economizar bateria.") { st.put(Keys.FX_WALL_ANIM, it) }
    Choice("Limite de quadros do papel de parede", listOf(15, 24, 30, 60).map { it to "$it fps" }, st.int(Keys.FX_FPS, 60)) { st.put(Keys.FX_FPS, it) }
    SliderRow("Vinheta", st.float(Keys.FX_VIGNETTE, 0.35f), 0f..1f, "${(st.float(Keys.FX_VIGNETTE, 0.35f) * 100).roundToInt()}%") { st.put(Keys.FX_VIGNETTE, it) }
    T("Limitação: o desfoque usa o papel de parede como fundo; janelas por baixo de uma janela translúcida aparecem sem desfoque.", 11.sp, b.dim, modifier = Modifier.padding(top = 6.dp))
}

@Composable
private fun PanelPage(session: DesktopSession) {
    val st = session.app.settings; val b = LocalBreeze.current
    SwitchRow("Painel flutuante", st.bool(Keys.PANEL_FLOATING, true), "Margem ao redor e cantos arredondados, como no Plasma 6.") { st.put(Keys.PANEL_FLOATING, it) }
    SwitchRow("Opacidade adaptativa", st.bool(Keys.PANEL_ADAPTIVE, true), "O painel fica opaco quando uma janela está maximizada.") { st.put(Keys.PANEL_ADAPTIVE, it) }
    SwitchRow("Painel no topo", st.bool(Keys.PANEL_TOP, false)) { st.put(Keys.PANEL_TOP, it) }
    if (st.bool(Keys.PANEL_FLOATING, true)) {
        val m = st.int(Keys.PANEL_MARGIN, U.DEFAULT_PANEL_MARGIN_DP)
        val r = st.int(Keys.PANEL_RADIUS, U.DEFAULT_PANEL_RADIUS_DP)
        val w = st.int(Keys.PANEL_WIDTH_PCT, U.DEFAULT_PANEL_WIDTH_PCT)
        SliderRow("Distância das bordas", m.toFloat(), 0f..48f, "$m dp") { st.put(Keys.PANEL_MARGIN, it.roundToInt()) }
        SliderRow("Arredondamento dos cantos", r.toFloat(), 0f..32f, "$r dp") { st.put(Keys.PANEL_RADIUS, it.roundToInt()) }
        SliderRow("Largura do painel", w.toFloat(), 40f..100f, if (w >= 100) "Total" else "$w% (centralizado)") { st.put(Keys.PANEL_WIDTH_PCT, ((it / 5).roundToInt() * 5)) }
        BreezeButton("Restaurar painel flutuante padrão", {
            st.put(Keys.PANEL_MARGIN, U.DEFAULT_PANEL_MARGIN_DP); st.put(Keys.PANEL_RADIUS, U.DEFAULT_PANEL_RADIUS_DP); st.put(Keys.PANEL_WIDTH_PCT, U.DEFAULT_PANEL_WIDTH_PCT)
        }, Modifier.padding(vertical = 6.dp))
    }
    SliderRow("Altura do painel", st.int(Keys.PANEL_H, U.DEFAULT_PANEL_DP).toFloat(), 34f..80f, "${st.int(Keys.PANEL_H, U.DEFAULT_PANEL_DP)} dp") { st.put(Keys.PANEL_H, it.roundToInt()) }
    SliderRow("Áreas de trabalho virtuais", st.int(Keys.DESKTOPS, 2).toFloat(), 1f..6f, "${st.int(Keys.DESKTOPS, 2)}") { st.put(Keys.DESKTOPS, it.roundToInt()) }
    SectionTitle("Usuário")
    var name by remember { mutableStateOf(st.string(Keys.USER_NAME, "Usuário")) }
    Box(Modifier.fillMaxWidth().clip(RoundedCornerShape(6.dp)).background(b.view).border(1.dp, b.line, RoundedCornerShape(6.dp)).padding(10.dp)) {
        BasicTextField(name, { name = it; st.put(Keys.USER_NAME, it) }, Modifier.fillMaxWidth(), singleLine = true,
            textStyle = TextStyle(color = b.text, fontSize = 13.sp), cursorBrush = SolidColor(b.accent))
    }
}

@Composable
private fun TerminalPage(session: DesktopSession) {
    val st = session.app.settings; val b = LocalBreeze.current
    SectionTitle("Esquema de cores")
    Row(Modifier.horizontalScroll(rememberScrollState())) {
        TermSchemes.ALL.forEach { s ->
            val on = st.string(Keys.TERM_SCHEME, "breeze") == s.id
            Column(Modifier.padding(end = 10.dp)) {
                Box(Modifier.size(130.dp, 64.dp).clip(RoundedCornerShape(6.dp)).background(s.bg).border(if (on) 3.dp else 1.dp, if (on) b.accent else b.line, RoundedCornerShape(6.dp))
                    .breezeClickable(6.dp) { st.put(Keys.TERM_SCHEME, s.id) }.padding(8.dp)) {
                    Column {
                        T("user@host:~$", 11.sp, s.ansi[2])
                        Row { (1..6).forEach { Box(Modifier.padding(top = 6.dp, end = 3.dp).size(14.dp, 8.dp).background(s.ansi[it])) } }
                    }
                }
                T(s.label, 11.sp, modifier = Modifier.padding(top = 3.dp))
            }
        }
    }
    SectionTitle("Aparência")
    SliderRow("Tamanho da fonte", st.float(Keys.TERM_FONT, 12f), 8f..28f, "${st.float(Keys.TERM_FONT, 12f).roundToInt()} sp") { st.put(Keys.TERM_FONT, it.roundToInt().toFloat()) }
    SliderRow("Opacidade do fundo", st.float(Keys.TERM_OPACITY, 0.82f), 0.3f..1f, "${(st.float(Keys.TERM_OPACITY, 0.82f) * 100).roundToInt()}%") { st.put(Keys.TERM_OPACITY, it) }
    SwitchRow("Executar fastfetch ao abrir", st.bool(Keys.TERM_FASTFETCH, true), "Mostra o logotipo do Android e dados reais do aparelho (CPU, GPU, OpenGL, memória…).") { st.put(Keys.TERM_FASTFETCH, it) }
    SectionTitle("Apps gráficos Linux")
    SwitchRow(
        "Usar X11 (servidor próprio do PlasmaDroid) em vez de Wayland nativo",
        st.string(Keys.GFX_SERVER, "wayland") == "x11",
        "Desligado: Wayland nativo (mais rápido, recomendado). Ligado: o PlasmaDroid roda um servidor X11 nativo; cada janela X11 vira uma janela do desktop, como as Wayland. " +
            "O Konsole exporta DISPLAY=:0. Também dá para iniciar manualmente com `plasma-x11 start`."
    ) { st.put(Keys.GFX_SERVER, if (it) "x11" else "wayland") }
    SectionTitle("Saída Wayland")
    SwitchRow(
        "Sincronizar resolução com a janela",
        st.bool(Keys.WAYLAND_SYNC_SIZE, false),
        "Ligado: a resolução de saída do Wayland acompanha o tamanho do desktop sempre que ele muda (arrastar, girar a tela). Desligado: resolução fixa em 1280×720."
    ) { st.put(Keys.WAYLAND_SYNC_SIZE, it) }
    BreezeButton("Abrir o Konsole", { session.openTool("terminal") }, Modifier.padding(top = 8.dp), primary = true)
}

@Composable
private fun ScalePage(session: DesktopSession) {
    val st = session.app.settings; val b = LocalBreeze.current
    val saved = st.float(Keys.UI_SCALE, Keys.DEFAULT_UI_SCALE).coerceIn(Keys.MIN_UI_SCALE, Keys.MAX_UI_SCALE)
    var pending by remember { mutableStateOf(false) }
    var prev by remember { mutableFloatStateOf(saved) }
    var left by remember { mutableIntStateOf(15) }
    LaunchedEffect(pending) {
        if (!pending) return@LaunchedEffect
        left = 15
        while (left > 0) { delay(1000); left-- }
        st.put(Keys.UI_SCALE, prev); pending = false
    }
    fun change(v: Float) { if (!pending && kotlin.math.abs(v - saved) > 0.001f) { prev = saved; st.put(Keys.UI_SCALE, v); pending = true } }
    SectionTitle("Escala da interface")
    Row(Modifier.horizontalScroll(rememberScrollState()).padding(vertical = 6.dp)) {
        listOf(0.5f, 0.6f, 0.75f, 0.9f, 1f, 1.15f, 1.25f, 1.5f, 1.75f, 2f).forEach { v ->
            val on = kotlin.math.abs(saved - v) < 0.01f
            Box(Modifier.padding(end = 6.dp).clip(RoundedCornerShape(6.dp)).background(if (on) b.accent else b.hover)
                .breezeClickable(6.dp) { change(v) }
                .padding(horizontal = 14.dp, vertical = 8.dp)) { T("${(v * 100).roundToInt()}%", 12.sp, if (on) b.onAccent else b.text) }
        }
    }
    SliderRow("Ajuste fino", saved, Keys.MIN_UI_SCALE..Keys.MAX_UI_SCALE, "${(saved * 100).roundToInt()}%") { change((it * 20).roundToInt() / 20f) }
    if (pending) Card {
        T("Manter esta escala? Voltando automaticamente em $left s…", 13.sp, weight = FontWeight.Medium)
        Row(Modifier.padding(top = 8.dp)) {
            BreezeButton("Manter", { pending = false }, primary = true)
            Spacer(Modifier.width(8.dp))
            BreezeButton("Reverter", { st.put(Keys.UI_SCALE, prev); pending = false })
        }
    }
}

@Composable
private fun GpuPage(session: DesktopSession) {
    val b = LocalBreeze.current
    val g by session.compositor.info.collectAsState()
    SectionTitle("Compositor ${g.backend}")
    Card {
        T(if (g.ready) "Ativo" else "Inativo${if (g.error.isNotEmpty()) " — ${g.error}" else ""}", 14.sp, if (g.ready) b.positive else b.negative, FontWeight.SemiBold)
        if (g.ready) {
            val fields = if (g.backend.startsWith("Vulkan")) listOf(
                "Dispositivo" to g.renderer, "API" to g.version, "Shaders" to g.glsl,
                "Backend" to "VkInstance · VkDevice · VkSwapchainKHR · vkQueueSubmit/vkQueuePresentKHR"
            ) else listOf(
                "Renderizador" to g.renderer, "Fabricante" to g.vendor, "Versão GL" to g.version, "GLSL" to g.glsl,
                "Textura máx." to "${g.maxTexture}px", "Quadros" to "%.0f fps".format(g.fps)
            )
            fields.forEach { (k, v) ->
                Row(Modifier.padding(top = 4.dp)) { T(k, 12.sp, b.dim, modifier = Modifier.width(110.dp)); T(v, 12.sp, maxLines = 3) }
            }
        }
    }
}

@Composable
private fun AboutPage(session: DesktopSession) {
    val ctx = androidx.compose.ui.platform.LocalContext.current
    val text by produceState("Carregando…", session) {
        value = runCatching {
            kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                Fastfetch.render(ctx, session.app, session, "user", android.os.Build.MODEL.lowercase().replace(' ', '-'), showLogo = true)
            }
        }.getOrElse { "Falha: ${it.message}" }
    }
    val scheme = TermSchemes.BREEZE
    Card {
        Column(Modifier.horizontalScroll(rememberScrollState())) {
            text.split('\n').forEach {
                androidx.compose.material3.Text(Ansi.render(it.ifEmpty { " " }, scheme),
                    style = TextStyle(fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace, fontSize = 11.sp, color = scheme.fg), softWrap = false)
            }
        }
    }
}
