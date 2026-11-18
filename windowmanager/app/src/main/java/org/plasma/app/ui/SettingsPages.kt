
package org.plasma.app.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.provider.Settings
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.plasma.app.android.PlasmaNotificationService
import org.plasma.app.session.DesktopSession
import org.plasma.bridge.ShizukuState
import org.plasma.core.Keys
import org.plasma.core.PlasmaLog
import org.plasma.wm.Orientation
import org.plasma.wm.WindowAspect
import org.plasma.render.RenderBackend
import org.plasma.sharing.CodecCaps
import org.plasma.sharing.NetworkInfoProvider
import org.plasma.sharing.VideoCodec
import kotlin.math.roundToInt

@Composable
internal fun SwitchRow(label: String, checked: Boolean, desc: String? = null, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) { T(label); if (desc != null) T(desc, 11.sp, LocalBreeze.current.dim) }
        BreezeSwitch(checked, onChange)
    }
}

@Composable
internal fun SliderRow(label: String, value: Float, range: ClosedFloatingPointRange<Float>, display: String, onChange: (Float) -> Unit) {
    Column(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        Row { T(label, modifier = Modifier.weight(1f)); T(display, 12.sp, LocalBreeze.current.dim) }
        BreezeSlider(value, onChange, range, Modifier.padding(top = 4.dp))
    }
}

@Composable
internal fun <V> Choice(label: String, options: List<Pair<V, String>>, selected: V, onSelect: (V) -> Unit) {
    val b = LocalBreeze.current
    Column(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        T(label, 12.sp, b.dim)
        Row(Modifier.horizontalScroll(rememberScrollState()).padding(top = 4.dp)) {
            options.forEach { (v, l) ->
                Box(Modifier.padding(end = 6.dp).clip(androidx.compose.foundation.shape.RoundedCornerShape(4.dp))
                    .background(if (v == selected) b.accent else b.hover).clickable { onSelect(v) }.padding(horizontal = 12.dp, vertical = 7.dp)) {
                    T(l, 12.sp, if (v == selected) Color.White else b.text)
                }
            }
        }
    }
}

@Composable
internal fun RenderPage(session: DesktopSession) {
    val app = session.app; val st = app.settings; val b = LocalBreeze.current
    val sel by app.render.selection.collectAsState()
    val scope = rememberCoroutineScope()
    var busy by remember { mutableStateOf(false) }
        Choice("Backend preferido", RenderBackend.entries.map { it.id to it.label }, st.string(Keys.RENDER, "auto")) { st.put(Keys.RENDER, it) }
    BreezeButton(if (busy) "Testando…" else "Detectar e testar agora", {
        busy = true; scope.launch { app.render.evaluate(); busy = false }
    }, enabled = !busy, primary = true)
    val s = sel
    if (s == null) T("Ainda sem resultado.", 12.sp, b.dim, modifier = Modifier.padding(top = 8.dp)) else {
        SectionTitle("Backend selecionado após a sonda")
        T("${s.active.label}  (pedido: ${s.requested.label})", 15.sp, weight = FontWeight.SemiBold)
        T("A sonda isolada valida dispositivo e submissão. Vulkan só é confirmado como compositor ativo depois que a Surface Android, a swapchain e os pipelines SPIR-V forem criados; veja o backend real na página Compositor e GPU.", 11.sp, b.dim)
        SectionTitle("Como a decisão foi tomada")
        s.log.forEach { BulletText(it) }
        s.probes[RenderBackend.VULKAN]?.details?.takeIf { it.isNotEmpty() }?.let { SectionTitle("Vulkan"); it.forEach { (k, v) -> T("$k = $v", 11.sp, b.dim) } }
        s.probes[RenderBackend.GLES]?.details?.takeIf { it.isNotEmpty() }?.let { SectionTitle("OpenGL ES / EGL"); it.forEach { (k, v) -> T("$k = $v", 11.sp, b.dim) } }
        SectionTitle("Sistema")
        s.systemInfo.forEach { (k, v) -> T("$k: $v", 12.sp) }
    }
    SectionTitle("Observação")
    T("O backend ativo é confirmado em runtime. Vulkan usa VkSurfaceKHR, swapchain, pipelines SPIR-V e submissão/presentação nativas; OpenGL ES usa EGL ES 3 com FBOs e blur Dual-Kawase. O conteúdo Compose e as janelas Android continuam na camada HWUI acima da superfície do compositor.", 11.sp, b.dim)
}

@Composable
internal fun DisplaysPage(session: DesktopSession) {
    val app = session.app; val st = app.settings; val b = LocalBreeze.current
    val list by app.displays.displays.collectAsState()
    val ctx = LocalContext.current
    SwitchRow("Abrir o desktop automaticamente ao conectar monitor externo", st.bool(Keys.AUTO_EXTERNAL, false)) { st.put(Keys.AUTO_EXTERNAL, it) }
    T("Somente displays que o Android expõe aparecem aqui. DisplayPort Alt Mode depende do hardware e do sistema; não é emulado.", 11.sp, b.dim)
    list.forEach { d ->
        SectionTitle("${d.name} (id ${d.id})${if (d.isDefault) " · principal" else ""}${if (d.isExternal) " · externo" else ""}")
        T("${d.width}×${d.height} @ ${"%.0f".format(d.fps)} Hz · ${d.densityDpi} dpi", 12.sp)
        if (d.isExternal) {
            Choice("Modo (resolução e taxa)", d.modes.take(8).map { it.id to it.label },
                st.int(Keys.displayMode(d.id), d.currentModeId)) { st.put(Keys.displayMode(d.id), it) }
            val running = app.sessions.all().any { it.displayId == d.id }
            Row {
                BreezeButton(if (running) "Desktop já aberto" else "Abrir desktop independente aqui", {
                    val err = app.displays.launchOn(Intent(ctx, org.plasma.app.DesktopActivity::class.java), d.id)
                    if (err != null) session.showToast("Falha: $err")
                }, enabled = !running, primary = true)
                Spacer(Modifier.width(8.dp))
                BreezeButton("Fechar desktop (voltar ao espelhamento do sistema)", { app.sessions.release(d.id) }, enabled = running)
            }
            T("O espelhamento/extensão em si é decidido pelo Android; fechar o desktop devolve o display ao sistema.", 11.sp, b.dim)
        }
    }
}

@Composable
internal fun FullscreenPage(session: DesktopSession) {
    val st = session.app.settings; val b = LocalBreeze.current
    val on = st.bool(Keys.FORCE_ASPECT, true)
    SectionTitle("Tela cheia de aplicativos Android e Linux")
    SwitchRow("Forçar proporção em tela cheia", on,
        "Em tela cheia, o app usa a proporção abaixo na orientação do KDE (paisagem ou retrato, conforme a tela do desktop); o resto fica preto. Desligado, o app ocupa a tela inteira.") { st.put(Keys.FORCE_ASPECT, it) }
    if (on) {
        Choice("Proporção", WindowAspect.PRESETS.map { it to (if (it == WindowAspect.SCREEN) "Igual à tela" else it) },
            st.string(Keys.ASPECT_RATIO, WindowAspect.DEFAULT)) { st.put(Keys.ASPECT_RATIO, it) }
        val o = session.kdeOrientation()
        T("Orientação atual do KDE: ${if (o == Orientation.LANDSCAPE) "paisagem" else "retrato"}. Ela muda sozinha quando você gira o aparelho ou troca de monitor.", 11.sp, b.dim)
    }
    T("O botão de tela cheia fica na barra do título dos apps Android e Linux (ou F11). Em tela cheia, toque na alça do topo para girar ou sair. O ícone de girar troca largura e altura da janela; em tela cheia, inverte a orientação da proporção forçada.", 11.sp, b.dim, modifier = Modifier.padding(top = 6.dp))
}

@Composable
internal fun AndroidPage(session: DesktopSession) {
    val app = session.app; val st = app.settings; val b = LocalBreeze.current
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val shz by app.shizuku.state.collectAsState()
    val root by app.root.available.collectAsState()
    val projection by org.plasma.bridge.ProjectionHolder.active.collectAsState()
    SectionTitle("Shizuku (opcional)")
    T(shz.label, 13.sp, if (shz == ShizukuState.CONNECTED) Color(0xFF27AE60) else b.text, FontWeight.Medium)
    T("Com Shizuku conectado: janelas de qualquer app em display confiável + mouse/teclado/toque dentro das janelas.", 11.sp, b.dim)
    Row(Modifier.padding(top = 6.dp)) {
        BreezeButton("Solicitar permissão", { app.shizuku.requestPermission() }, enabled = shz == ShizukuState.NO_PERMISSION, primary = true)
        Spacer(Modifier.width(6.dp))
        BreezeButton("Reavaliar", { app.shizuku.refresh() })
        Spacer(Modifier.width(6.dp))
        BreezeButton("Abrir Shizuku", { ctx.packageManager.getLaunchIntentForPackage("moe.shizuku.privileged.api")?.let { ctx.startActivity(it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) } ?: session.showToast("Shizuku não instalado") })
    }
    SectionTitle("Root (opcional)")
    T(when (root) { true -> "Root disponível"; false -> "Sem root"; null -> "Não testado" }, 13.sp)
    Row(Modifier.padding(top = 6.dp)) {
        BreezeButton("Detectar root", { scope.launch { app.root.detect() } })
        Spacer(Modifier.width(8.dp))
    }
    SwitchRow("Usar root como backend quando não houver Shizuku", st.bool(Keys.ROOT_ON, false), "Sem display confiável; entrada via comando `input` (mais lenta).") { st.put(Keys.ROOT_ON, it) }
    SectionTitle("Janelas Android")
    Choice("Método preferido", listOf("auto" to "Automático", "shizuku" to "Shizuku (B)", "app" to "Display do app (A)", "conventional" to "Convencional (E)"),
        st.string(Keys.METHOD, "auto")) { st.put(Keys.METHOD, it) }
    SliderRow("Densidade das janelas", st.int(Keys.ANDROID_DPI, 240).toFloat(), 120f..420f, "${st.int(Keys.ANDROID_DPI, 240)} dpi") { st.put(Keys.ANDROID_DPI, it.roundToInt()) }
    SwitchRow("Forçar atividades redimensionáveis", st.bool(Keys.FORCE_RESIZABLE, false),
        "Altera a configuração global force_resizable_activities (exige Shizuku/root). Reversível aqui.") { on ->
        scope.launch {
            val err = withContext(Dispatchers.IO) { app.backends.setForceResizable(on) }
            if (err == null) st.put(Keys.FORCE_RESIZABLE, on) else session.showToast(err)
        }
    }
    SectionTitle("Captura de tela (Método D)")
    T(if (projection) "Autorizada" else "Não autorizada", 13.sp)
    Row(Modifier.padding(top = 6.dp)) {
        BreezeButton("Autorizar captura", { session.requestProjection() }, enabled = !projection)
        Spacer(Modifier.width(6.dp))
        BreezeButton("Abrir janela de captura", { session.openTool("capture") })
    }
    SectionTitle("Notificações")
    T(if (PlasmaNotificationService.instance != null) "Acesso concedido" else "Acesso não concedido", 13.sp)
    BreezeButton("Abrir configuração de acesso", { ctx.startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }, Modifier.padding(top = 6.dp))
    SectionTitle("Compatibilidade por aplicativo (resultado real dos testes)")
    val keys = st.keysWithPrefix("compat.")
    if (keys.isEmpty()) T("Ainda nenhum aplicativo foi aberto em janela.", 12.sp, b.dim)
    keys.sorted().forEach { k ->
        Row(Modifier.fillMaxWidth().padding(vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
            T(k.removePrefix("compat."), 12.sp, modifier = Modifier.weight(1f), maxLines = 1)
            T(when (st.string(k)) { "B" -> "B · Shizuku"; "A" -> "A · display do app"; else -> "E · convencional" }, 12.sp, b.accent)
            Box(Modifier.clickable { st.remove(k) }.padding(8.dp)) { T("limpar", 11.sp, b.dim) }
        }
    }
}

@Composable
internal fun SharingPage(session: DesktopSession) {
    val app = session.app; val st = app.settings; val b = LocalBreeze.current
    val stats by app.sharing.stats.collectAsState()
    val caps = remember { CodecCaps.query() }
    val addrs = remember(stats.running) { NetworkInfoProvider.addresses() }
    var err by remember { mutableStateOf("") }
    val port = st.int(Keys.SHARE_PORT, 47800)
    T("Transmite o desktop em H.264/H.265/AV1 por TCP criptografado (AES-256-GCM, chave derivada do PIN). Use tools/plasma_receiver.py + ffplay no computador.", 11.sp, b.dim)
    SectionTitle("Qualidade")
    Choice("Resolução", listOf((1280 to 720) to "720p", (1920 to 1080) to "1080p", (2560 to 1440) to "1440p"), st.int(Keys.SHARE_W, 1280) to st.int(Keys.SHARE_H, 720)) { st.put(Keys.SHARE_W, it.first); st.put(Keys.SHARE_H, it.second) }
    Choice("FPS", listOf(15, 24, 30, 60).map { it to "$it" }, st.int(Keys.SHARE_FPS, 30)) { st.put(Keys.SHARE_FPS, it) }
    SliderRow("Bitrate", st.int(Keys.SHARE_KBPS, 6000).toFloat(), 1000f..30000f, "${st.int(Keys.SHARE_KBPS, 6000) / 1000f} Mbps") { st.put(Keys.SHARE_KBPS, (it / 500).roundToInt() * 500) }
    Choice("Codec", caps.filter { it.encoderName != null }.map { it.codec.id to "${it.codec.label}${if (it.hardware) " · HW" else " · SW"}" }, st.int(Keys.SHARE_CODEC, 1)) { st.put(Keys.SHARE_CODEC, it) }
    caps.filter { it.encoderName == null }.forEach { T("${it.codec.label}: sem encoder neste aparelho", 11.sp, b.dim) }
    SwitchRow("Baixa latência", st.bool(Keys.SHARE_LOWLAT, true)) { st.put(Keys.SHARE_LOWLAT, it) }
    SectionTitle("Conexão")
    var pin by remember { mutableStateOf(st.string(Keys.SHARE_PIN, "1234")) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        T("PIN:", modifier = Modifier.padding(end = 8.dp))
        Box(Modifier.width(140.dp).background(b.view).border(1.dp, b.line).padding(8.dp)) {
            BasicTextField(pin, { pin = it; st.put(Keys.SHARE_PIN, it) }, singleLine = true, textStyle = TextStyle(color = b.text, fontSize = 13.sp), cursorBrush = SolidColor(b.accent))
        }
        T("  Porta: $port", 12.sp, b.dim)
    }
    SectionTitle("Endereços")
    if (addrs.isEmpty()) T("Sem rede ativa.", 12.sp, b.dim)
    addrs.forEach { T("${it.kind} (${it.iface}): ${it.ip}:$port", 12.sp) }
    T("USB: ative o tethering USB e use o IP \"USB\" acima, ou rode `adb forward tcp:$port tcp:$port` e conecte em 127.0.0.1.", 11.sp, b.dim)
    SectionTitle("Estado")
    Row {
        BreezeButton(if (stats.running) "Parar" else "Iniciar", {
            if (stats.running) app.sharing.stop() else {
                val src = session.frameSource
                err = if (src == null) "Fonte de vídeo indisponível" else app.sharing.start(src) ?: ""
            }
        }, primary = !stats.running)
    }
    if (err.isNotEmpty()) T(err, 12.sp, Color(0xFFDA4453))
    if (stats.running) {
        T("Clientes: ${stats.clients} · ${"%.0f".format(stats.fps)} fps · ${stats.kbps} kbps", 13.sp, weight = FontWeight.Medium)
        T("Quadros: ${stats.framesEncoded} · descartados: ${stats.framesDropped} · enviado: ${stats.bytesSent / 1024} KB", 11.sp, b.dim)
        if (stats.lastError.isNotEmpty()) T("Erro: ${stats.lastError}", 11.sp, Color(0xFFDA4453))
        T("Mantenha esta tela do desktop visível: o vídeo é gerado a partir dela. Áudio e controle remoto não estão implementados.", 11.sp, b.dim)
    }
}

@Composable
internal fun SystemPage(session: DesktopSession) {
    val app = session.app; val b = LocalBreeze.current
    val ctx = LocalContext.current
    val log by PlasmaLog.entries.collectAsState()
    var report by remember { mutableStateOf<List<String>>(emptyList()) }
    val scope = rememberCoroutineScope()
    SectionTitle("Desempenho")
    SystemMonitorView(session)
    SectionTitle("Diagnóstico")
    BreezeButton("Executar diagnóstico", {
        scope.launch {
            val sel = app.render.evaluate()
            val shz = app.shizuku.state.value
            report = buildList {
                add("Compositor/WM: ${session.wm.state.value.windows.size} janela(s), ${session.wm.state.value.desktopCount} áreas de trabalho — OK")
                add("Render: ativo=${sel.active.label}; " + sel.probes.values.joinToString { "${it.backend.label}=${if (it.usable) "OK" else "falhou"}" })
                add("Displays: ${app.displays.displays.value.size} (${app.displays.externals().size} externo(s))")
                add("Shizuku: ${shz.label}")
                add("Root: ${app.root.available.value ?: "não testado"}")
                add("Captura (MediaProjection): ${org.plasma.bridge.ProjectionHolder.active.value}")
                add("Notificações: ${if (PlasmaNotificationService.instance != null) "acesso concedido" else "sem acesso"}")
                add("Codecs: " + CodecCaps.query().joinToString { "${it.codec.label}=${it.encoderName ?: "não"}${if (it.hardware) "(HW)" else ""}" })
                add("Rede: " + NetworkInfoProvider.addresses().joinToString { "${it.kind}:${it.ip}" }.ifEmpty { "sem rede" })
                add("Arquivos: " + if (android.os.Build.VERSION.SDK_INT < 30 || android.os.Environment.isExternalStorageManager()) "acesso total" else "acesso limitado")
            }
            PlasmaLog.i("diag", report.joinToString(" | "))
        }
    }, primary = true)
    report.forEach { BulletText(it) }
    SectionTitle("Logs (${log.size})")
    Row {
        BreezeButton("Copiar", { (ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText("plasma-log", PlasmaLog.dump())); session.showToast("Logs copiados") })
        Spacer(Modifier.width(6.dp))
        BreezeButton("Compartilhar", {
            ctx.startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, PlasmaLog.dump()), "Logs").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        })
    }
    Column(Modifier.padding(top = 8.dp).fillMaxWidth().heightIn(max = 360.dp).background(b.view).verticalScroll(rememberScrollState()).padding(6.dp)) {
        log.takeLast(200).forEach { e ->
            T(PlasmaLog.format(e), 10.sp, when (e.level) { 'E' -> Color(0xFFDA4453); 'W' -> Color(0xFFF67400); else -> b.dim })
        }
    }
}

@Composable
internal fun ShortcutsPage() {
    val b = LocalBreeze.current
    listOf("Super (Meta)" to "Abrir/fechar o menu", "Alt + Tab (Shift)" to "Alternar janelas", "Alt + F4" to "Fechar a janela ativa", "Super + D" to "Mostrar a área de trabalho",
        "Super + L" to "Bloquear o desktop", "Ctrl + Alt + T" to "Abrir o Konsole", "Super + Seta esq. / dir." to "Ancorar à esquerda / direita",
        "Super + Seta cima / baixo" to "Maximizar / restaurar", "F11" to "Tela cheia da janela ativa (Esc sai)", "Ctrl + Alt + Seta esq. / dir." to "Trocar de área de trabalho", "Botão direito do mouse" to "Menu de contexto (em janela Android = Voltar)")
        .forEach { (k, v) -> Row(Modifier.padding(vertical = 3.dp)) { T(k, 13.sp, b.accent, FontWeight.Medium, Modifier.width(190.dp)); T(v, 13.sp) } }
    T("Alguns aparelhos interceptam Alt+Tab e Super antes do app receber a tecla.", 11.sp, b.dim, modifier = Modifier.padding(top = 10.dp))
}
