
package org.plasma.app.ui

import android.content.Intent
import android.net.Uri
import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.plasma.app.R
import org.plasma.app.session.Ansi
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.TermSchemes
import org.plasma.bridge.ShizukuState
import org.plasma.core.Keys
import org.plasma.proot.SetupStep
import org.plasma.proot.StepState
import org.plasma.render.RenderBackend
import kotlin.math.roundToInt

private const val PAGES = 5

@Composable
fun SetupWindow(session: DesktopSession) {
    val app = session.app
    val b = LocalBreeze.current
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var page by remember { mutableIntStateOf(0) }
    val ui by app.debian.ui.collectAsState()

    fun startEnvironment() {
        if (app.debian.ui.value.running) return
        scope.launch { app.debian.setup() }
    }

    LaunchedEffect(page) { if (page == 2 && !app.debian.ready) startEnvironment() }

    fun finish() {
        context.getSharedPreferences("plasma_first_run", 0).edit().putBoolean("completed", true).apply()
        session.wm.state.value.windows.firstOrNull { it.appId == "tool:setup" }?.let { session.closeWindow(it.id) }
    }

    Column(Modifier.fillMaxSize().background(b.window)) {
        Crossfade(page, Modifier.weight(1f).fillMaxWidth(), animationSpec = tween(160), label = "setup-page") { p ->
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState(), enabled = true)) {
                Hero(p)
                Column(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 14.dp)) {
                    when (p) {
                        0 -> WelcomePage()
                        1 -> PermissionsPage(session)
                        2 -> EnvironmentPage(session, onRetry = { startEnvironment() })
                        3 -> LookPage(session)
                        else -> DonePage(session)
                    }
                }
            }
        }
        Footer(page, nextEnabled = !(page == 2 && ui.running), onBack = { if (page > 0) page-- },
            onNext = { if (page < PAGES - 1) page++ else finish() })
    }
}

@Composable
private fun Footer(page: Int, nextEnabled: Boolean, onBack: () -> Unit, onNext: () -> Unit) {
    val b = LocalBreeze.current
    Column(Modifier.fillMaxWidth().background(b.window)) {
        Box(Modifier.fillMaxWidth().height(1.dp).background(b.line))
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.width(96.dp)) { if (page > 0) BreezeButton("Voltar", onBack) }
            Spacer(Modifier.weight(1f))
            PageDots(PAGES, page)
            Spacer(Modifier.weight(1f))
            Box(Modifier.width(96.dp), contentAlignment = Alignment.CenterEnd) {
                BreezeButton(if (page == PAGES - 1) "Concluir" else "Continuar", onNext, primary = true, enabled = nextEnabled)
            }
        }
    }
}

@Composable
private fun PageDots(count: Int, current: Int) {
    val b = LocalBreeze.current
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
        for (i in 0 until count) {
            val w by animateFloatAsState(if (i == current) 18f else 7f, tween(180), label = "dot")
            Box(Modifier.height(7.dp).width(w.dp).clip(RoundedCornerShape(4.dp)).background(if (i == current) b.accent else b.line))
        }
    }
}

@Composable
private fun Hero(page: Int) {
    val b = LocalBreeze.current
    val top = lerp(b.accent, Color.Black, if (b.dark) 0.6f else 0.3f)
    val mid = lerp(b.accent, b.window, 0.55f)
    Box(Modifier.fillMaxWidth().height(180.dp)) {
        Canvas(Modifier.fillMaxSize()) {
            drawRect(Brush.verticalGradient(listOf(top, mid, b.window)))
            val c1 = Offset(size.width * 0.82f, size.height * 0.18f)
            drawCircle(Brush.radialGradient(listOf(b.accent.copy(alpha = 0.55f), Color.Transparent), center = c1, radius = size.width * 0.5f), radius = size.width * 0.5f, center = c1)
            val c2 = Offset(size.width * 0.12f, size.height * 0.78f)
            drawCircle(Brush.radialGradient(listOf(Color.White.copy(alpha = 0.16f), Color.Transparent), center = c2, radius = size.width * 0.4f), radius = size.width * 0.4f, center = c2)
            for (i in 1..3) drawCircle(Color.White.copy(alpha = 0.06f), radius = size.height * (0.35f + 0.22f * i), center = Offset(size.width * 0.5f, size.height * 1.15f), style = Stroke(1.5f))
        }
        Box(Modifier.align(Alignment.Center)) { HeroArt(page) }
    }
}

@Composable
private fun GlassCircle(size: Dp, content: @Composable () -> Unit) {
    Box(
        Modifier.size(size).clip(CircleShape).background(Color.White.copy(alpha = 0.16f)).border(1.dp, Color.White.copy(alpha = 0.32f), CircleShape),
        contentAlignment = Alignment.Center,
    ) { content() }
}

@Composable
private fun HeroArt(page: Int) {
    when (page) {
        0 -> GlassCircle(96.dp) {
            Image(painterResource(R.drawable.ic_plasma_glyph), null, Modifier.size(54.dp), colorFilter = ColorFilter.tint(Color.White))
        }
        1 -> Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            listOf("folder", "android", "lock").forEach { GlassCircle(58.dp) { BreezeIcon(it, 28.dp, Color.White) } }
        }
        2 -> Column(Modifier.width(210.dp).clip(RoundedCornerShape(9.dp)).background(Color(0xE6101214)).border(1.dp, Color.White.copy(alpha = 0.18f), RoundedCornerShape(9.dp))) {
            Row(Modifier.fillMaxWidth().background(Color(0xFF272C31)).padding(horizontal = 9.dp, vertical = 6.dp), horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                listOf(0xFFED1515, 0xFFFDBC4B, 0xFF11D116).forEach { Box(Modifier.size(7.dp).clip(CircleShape).background(Color(it))) }
            }
            Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
                Box(Modifier.width(110.dp).height(5.dp).clip(RoundedCornerShape(3.dp)).background(Color(0xFF11D116)))
                Box(Modifier.width(150.dp).height(5.dp).clip(RoundedCornerShape(3.dp)).background(Color.White.copy(alpha = 0.55f)))
                Box(Modifier.width(80.dp).height(5.dp).clip(RoundedCornerShape(3.dp)).background(Color(0xFF3DAEE9)))
                Box(Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)).background(Color.White.copy(alpha = 0.15f))) {
                    Box(Modifier.fillMaxWidth(0.68f).fillMaxHeight().background(Color(0xFF3DAEE9)))
                }
            }
        }
        3 -> Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
            ThemeThumb(Themes.ALL[1], 78.dp, 56.dp)
            ThemeThumb(Themes.ALL[0], 96.dp, 68.dp)
            ThemeThumb(Themes.ALL[3], 78.dp, 56.dp)
        }
        else -> GlassCircle(96.dp) { BreezeIcon("check", 48.dp, Color.White) }
    }
}

@Composable
private fun ThemeThumb(t: GlobalTheme, w: Dp, h: Dp) {
    Canvas(Modifier.size(w, h)) {
        val r = CornerRadius(7.dp.toPx())
        drawRoundRect(Color(t.window), size = size, cornerRadius = r)
        drawRoundRect(Color(t.titleOn), size = Size(size.width, size.height * 0.2f), cornerRadius = CornerRadius(7.dp.toPx(), 7.dp.toPx()))
        drawRect(Color(t.view), topLeft = Offset(size.width * 0.06f, size.height * 0.28f), size = Size(size.width * 0.88f, size.height * 0.5f))
        drawRoundRect(Color(t.accent), topLeft = Offset(size.width * 0.1f, size.height * 0.34f), size = Size(size.width * 0.34f, size.height * 0.08f), cornerRadius = CornerRadius(3.dp.toPx()))
        drawRoundRect(Color(t.dim), topLeft = Offset(size.width * 0.1f, size.height * 0.5f), size = Size(size.width * 0.6f, size.height * 0.05f), cornerRadius = CornerRadius(3.dp.toPx()))
        drawRoundRect(Color(t.dim).copy(alpha = 0.6f), topLeft = Offset(size.width * 0.1f, size.height * 0.6f), size = Size(size.width * 0.45f, size.height * 0.05f), cornerRadius = CornerRadius(3.dp.toPx()))
        drawRoundRect(Color(t.line), size = size, cornerRadius = r, style = Stroke(1.dp.toPx()))
    }
}

@Composable
private fun PageTitle(title: String, subtitle: String) {
    val b = LocalBreeze.current
    T(title, 22.sp, weight = FontWeight.SemiBold)
    T(subtitle, 13.sp, b.dim, modifier = Modifier.padding(top = 6.dp, bottom = 12.dp))
}

@Composable
private fun FeatureCard(icon: String, title: String, text: String) {
    val b = LocalBreeze.current
    Row(
        Modifier.fillMaxWidth().padding(vertical = 5.dp).clip(RoundedCornerShape(9.dp)).background(b.view.copy(alpha = .68f))
            .border(1.dp, b.line, RoundedCornerShape(9.dp)).padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(38.dp).clip(CircleShape).background(b.accent.copy(alpha = 0.18f)), contentAlignment = Alignment.Center) { BreezeIcon(icon, 20.dp, b.accent) }
        Column(Modifier.weight(1f).padding(start = 12.dp)) {
            T(title, 14.sp, weight = FontWeight.SemiBold)
            T(text, 12.sp, b.dim, modifier = Modifier.padding(top = 2.dp))
        }
    }
}

@Composable
private fun WelcomePage() {
    PageTitle("Bem-vindo ao Plasma", "Vamos deixar o seu desktop KDE pronto em poucos passos.")
    FeatureCard("sparkle", "Simples por padrão", "Painel, menu Kickoff, janelas e temas do Breeze, como no KDE Plasma do PC.")
    FeatureCard("terminal", "Poderoso quando precisar", "Um Debian de verdade com Konsole, apt e aplicativos Linux em janelas Wayland.")
    FeatureCard("android", "Apps Android em janelas", "Abra seus aplicativos do Android lado a lado com os do Linux.")
}

@Composable
private fun PermissionsPage(session: DesktopSession) {
    val app = session.app
    val b = LocalBreeze.current
    val context = LocalContext.current
    val shizuku by app.shizuku.state.collectAsState()
    var storage by remember { mutableStateOf(hasStorageAccess(context)) }
    var notif by remember { mutableStateOf(hasNotificationListenerAccess(context)) }
    LaunchedEffect(Unit) {
        while (true) { storage = hasStorageAccess(context); notif = hasNotificationListenerAccess(context); delay(700) }
    }

    PageTitle("Permissões", "O desktop funciona sem elas, mas cada uma libera um recurso.")
    SetupPermissionRow(
        "Armazenamento",
        if (storage) "Acesso a /sdcard liberado" else "Dolphin e /sdcard no Debian",
        if (storage) "Concedido" else "Opcional",
        storage,
        if (storage) "OK" else "Configurar",
    ) { session.requestStorage() }

    SetupPermissionRow(
        "Shizuku",
        shizukuDetail(shizuku),
        shizuku.label,
        shizuku == ShizukuState.CONNECTED,
        when (shizuku) {
            ShizukuState.CONNECTED -> "Conectado"
            ShizukuState.NO_PERMISSION -> "Permitir"
            ShizukuState.NOT_INSTALLED -> "Obter"
            ShizukuState.NOT_RUNNING -> "Abrir"
            else -> "Reavaliar"
        },
    ) {
        when (shizuku) {
            ShizukuState.NO_PERMISSION -> { app.shizuku.requestPermission(); app.shizuku.refresh() }
            ShizukuState.NOT_INSTALLED -> runCatching { context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://shizuku.rikka.app/"))) }
            ShizukuState.NOT_RUNNING -> context.packageManager.getLaunchIntentForPackage("moe.shizuku.privileged.api")
                ?.let { context.startActivity(it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) } ?: app.shizuku.refresh()
            else -> app.shizuku.refresh()
        }
    }

    SetupPermissionRow(
        "Notificações",
        "Alertas na bandeja",
        if (notif) "Concedido" else "Opcional",
        notif,
        if (notif) "Ativo" else "Abrir",
    ) { session.openNotificationSettings() }

    T("O Shizuku é necessário para abrir apps Android em janelas.", 11.sp, b.dim, modifier = Modifier.padding(top = 8.dp))
}

private fun shizukuDetail(state: ShizukuState): String = when (state) {
    ShizukuState.CONNECTED -> "Janelas Android com toque e teclado"
    ShizukuState.NO_PERMISSION -> "Permita o acesso para abrir apps em janelas"
    ShizukuState.NOT_INSTALLED -> "Instale o Shizuku para abrir apps em janelas"
    ShizukuState.NOT_RUNNING -> "Abra o Shizuku para continuar"
    ShizukuState.CONNECTING -> "Conectando…"
    ShizukuState.ERROR -> "Falha na conexão. Tente de novo."
}

@Composable
private fun EnvironmentPage(session: DesktopSession, onRetry: () -> Unit) {
    val app = session.app
    val b = LocalBreeze.current
    val ui by app.debian.ui.collectAsState()
    var details by remember { mutableStateOf(false) }

    PageTitle("Preparando o ambiente", "Instalando o Debian e configurando Wayland e o Graphics Environment (Vulkan e PlasmaGL).")

    val total = ui.steps.size.coerceAtLeast(1)
    val done = ui.steps.count { it.state == StepState.DONE }
    val frac by animateFloatAsState(done.toFloat() / total, tween(300), label = "env-progress")
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(bottom = 10.dp)) {
        Box(Modifier.weight(1f).height(8.dp).clip(RoundedCornerShape(4.dp)).background(b.line)) {
            Box(Modifier.fillMaxWidth(frac).fillMaxHeight().clip(RoundedCornerShape(4.dp)).background(b.accent))
        }
        T("$done/$total", 12.sp, b.dim, modifier = Modifier.padding(start = 10.dp))
    }

    Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(9.dp)).background(b.view.copy(alpha = .68f)).border(1.dp, b.line, RoundedCornerShape(9.dp)).padding(12.dp)) {
        ui.steps.forEach { StepRow(it) }
    }
    if (ui.error != null) {
        T("Erro: ${ui.error}", 12.sp, Color(0xFFDA4453), modifier = Modifier.padding(top = 8.dp))
        Row(Modifier.padding(top = 8.dp)) { BreezeButton("Tentar de novo", onRetry, enabled = !ui.running) }
    }

    if (ui.log.isNotEmpty() && !ui.complete) {
        val ls = rememberLazyListState()
        LaunchedEffect(ui.log.size) { ls.scrollToItem((ui.log.size - 1).coerceAtLeast(0)) }
        val mono = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 10.sp, color = b.dim)
        Box(Modifier.padding(top = 10.dp).fillMaxWidth().height(120.dp).clip(RoundedCornerShape(6.dp)).background(b.view)) {
            LazyColumn(Modifier.fillMaxSize().padding(6.dp), state = ls) {
                items(ui.log) { androidx.compose.material3.Text(it, style = mono, maxLines = 1) }
            }
        }
    }

    GraphicsEnvironmentSection(session, Modifier.padding(top = 6.dp))

    Row(
        Modifier.fillMaxWidth().padding(top = 12.dp).clip(RoundedCornerShape(7.dp)).breezeClickable(7.dp) { details = !details }.padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        T("Detalhes técnicos", 13.sp, weight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
        BreezeIcon(if (details) "arrowUp" else "chevR", 14.dp, b.dim)
    }
    if (details) {
        SectionTitle("Wayland")
        val wl by app.wayland.state.collectAsState()
        val wlWindows by app.wayland.windows.collectAsState()
        Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(7.dp)).background(b.view.copy(alpha = .68f)).border(1.dp, b.line, RoundedCornerShape(7.dp)).padding(11.dp)) {
            StepRow(
                SetupStep(
                    id = "wayland", label = "Servidor Wayland",
                    state = when { wl.running && wl.outputAttached -> StepState.DONE; wl.running -> StepState.RUNNING; else -> StepState.PENDING },
                    detail = wl.status,
                )
            )
            if (wl.socketPath.isNotBlank()) T("Socket: ${wl.socketPath}", 11.sp, b.dim, maxLines = 2)
            T("XDG_RUNTIME_DIR no Debian: /run/plasma-wayland · WAYLAND_DISPLAY=wayland-0", 11.sp, b.dim, modifier = Modifier.padding(top = 2.dp))
            T("Janelas Linux: ${wlWindows.size}", 11.sp, b.dim)
        }

        if (ui.fastfetch.isNotBlank()) {
            SectionTitle("Fastfetch")
            val scheme = TermSchemes.byId("breeze")
            val mono = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 11.sp, lineHeight = 14.sp, color = scheme.fg)
            Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(6.dp)).background(scheme.bg).padding(8.dp)) {
                ui.fastfetch.lines().forEach { androidx.compose.material3.Text(Ansi.render(it.ifEmpty { " " }, scheme), style = mono, softWrap = false) }
            }
        }
    }
}

@Composable
private fun LookPage(session: DesktopSession) {
    val st = session.app.settings
    val b = LocalBreeze.current
    st.watch()
    val current = st.string(Keys.THEME, "breeze-dark")
    PageTitle("Aparência", "Escolha o tema e a cor de destaque. Dá para mudar depois nas Configurações do Sistema.")

    Themes.ALL.chunked(2).forEach { row ->
        Row(Modifier.fillMaxWidth().padding(bottom = 10.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            row.forEach { t ->
                val on = t.id == current
                Column(
                    Modifier.weight(1f).clip(RoundedCornerShape(10.dp)).background(b.view.copy(alpha = .68f))
                        .border(if (on) 2.dp else 1.dp, if (on) b.accent else b.line, RoundedCornerShape(10.dp))
                        .breezeClickable(10.dp) { st.put(Keys.THEME, t.id); st.put(Keys.ACCENT, U.THEME_ACCENT); st.put(Keys.WALLPAPER, "theme") }
                        .padding(10.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    ThemeThumb(t, 120.dp, 76.dp)
                    Row(Modifier.padding(top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                        if (on) BreezeIcon("check", 13.dp, b.accent)
                        T(t.label, 12.sp, if (on) b.accent else b.text, FontWeight.Medium, maxLines = 1, modifier = Modifier.padding(start = if (on) 4.dp else 0.dp))
                    }
                }
            }
            if (row.size == 1) Spacer(Modifier.weight(1f))
        }
    }

    SectionTitle("Cor de destaque")
    val accentRaw = st.int(Keys.ACCENT, U.THEME_ACCENT)
    Row(Modifier.fillMaxWidth().padding(vertical = 6.dp), horizontalArrangement = Arrangement.spacedBy(9.dp)) {
        ACCENTS.forEach { c ->
            val on = accentRaw == c.toInt()
            Box(
                Modifier.size(26.dp).clip(CircleShape).background(Color(c)).border(if (on) 2.dp else 0.dp, b.text, CircleShape)
                    .breezeClickable(13.dp) { st.put(Keys.ACCENT, c.toInt()) },
                contentAlignment = Alignment.Center,
            ) { if (on) BreezeIcon("check", 12.dp, Color.White) }
        }
    }

    SectionTitle("Desempenho")
    Choice("Renderizador", RenderBackend.entries.map { it.id to it.label }, st.string(Keys.RENDER, "auto")) { st.put(Keys.RENDER, it) }
    Choice("Limite de quadros", listOf(15, 24, 30, 60).map { it to "$it fps" }, st.int(Keys.FX_FPS, 60)) { st.put(Keys.FX_FPS, it) }
    SwitchRow("Desfoque do fundo", st.bool(Keys.FX_BLUR, true)) { st.put(Keys.FX_BLUR, it) }
    SwitchRow("Papel de parede animado", st.bool(Keys.FX_WALL_ANIM, true)) { st.put(Keys.FX_WALL_ANIM, it) }
    val speed = st.float(Keys.FX_ANIM_SPEED, 1f)
    SliderRow("Animações", speed, 0f..2f, if (speed <= 0.01f) "Desligadas" else "%.1fx".format(speed)) {
        st.put(Keys.FX_ANIM_SPEED, (it * 10).roundToInt() / 10f)
    }
}

@Composable
private fun DonePage(session: DesktopSession) {
    val app = session.app
    val context = LocalContext.current
    val shizuku by app.shizuku.state.collectAsState()
    val st = app.settings
    st.watch()
    PageTitle("Tudo pronto!", "Seu Plasma está configurado. Estes são os pontos principais:")
    FeatureCard("terminal", "Debian", if (app.debian.ready) "Instalado e pronto no Konsole" else "Ainda não finalizado — volte ao passo Ambiente")
    FeatureCard("android", "Apps Android", if (shizuku == ShizukuState.CONNECTED) "Shizuku conectado: janelas com toque e teclado" else "Sem Shizuku: apps Android não abrem em janelas")
    FeatureCard("folder", "Armazenamento", if (hasStorageAccess(context)) "/sdcard disponível no Dolphin e no Debian" else "Sem acesso ao /sdcard")
    FeatureCard("palette", "Tema", Themes.byId(st.string(Keys.THEME, "breeze-dark")).label)
}

@Composable
private fun StepRow(s: SetupStep) {
    val b = LocalBreeze.current
    val (mark, col) = when (s.state) {
        StepState.DONE -> "✔" to Color(0xFF27AE60)
        StepState.RUNNING -> "●" to b.accent
        StepState.FAILED -> "✘" to Color(0xFFDA4453)
        StepState.PENDING -> "○" to b.dim
    }
    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.Top) {
        T(mark, 14.sp, col, modifier = Modifier.width(22.dp))
        Column(Modifier.weight(1f)) {
            T(s.label, 13.sp, if (s.state == StepState.PENDING) b.dim else b.text, FontWeight.Medium)
            if (s.detail.isNotBlank()) T(s.detail, 11.sp, b.dim)
        }
    }
}
