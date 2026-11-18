
package org.plasma.app.ui

import android.media.AudioManager
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.plasma.app.session.DesktopSession
import org.plasma.app.system.BrightnessResult
import org.plasma.app.system.BtDev
import org.plasma.app.system.WifiNet

@Composable
fun TrayPanelPopup(modifier: Modifier, content: @Composable ColumnScope.() -> Unit) {
    PopupSurface(modifier.width(340.dp).heightIn(max = 540.dp)) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(12.dp), content = content)
    }
}

@Composable
private fun PanelHeader(icon: String, title: String, subtitle: String, trailing: @Composable RowScope.() -> Unit) {
    val b = LocalBreeze.current
    Row(Modifier.fillMaxWidth().padding(bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        BreezeIcon(icon, 22.dp, b.accent)
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            T(title, 15.sp, weight = FontWeight.SemiBold)
            T(subtitle, 11.sp, b.dim, maxLines = 1)
        }
        trailing()
    }
}

@Composable
private fun IconButton(icon: String, enabled: Boolean = true, onClick: () -> Unit) {
    val b = LocalBreeze.current
    Box(Modifier.size(30.dp).breezeClickable(5.dp, enabled = enabled) { onClick() }, contentAlignment = Alignment.Center) {
        BreezeIcon(icon, 16.dp, if (enabled) b.text else b.dim.copy(alpha = .5f))
    }
}

@Composable
private fun InfoCard(content: @Composable ColumnScope.() -> Unit) {
    val b = LocalBreeze.current
    Column(Modifier.fillMaxWidth().padding(vertical = 4.dp).clip(RoundedCornerShape(8.dp)).background(b.view.copy(alpha = .6f))
        .border(1.dp, b.line.copy(alpha = .6f), RoundedCornerShape(8.dp)).padding(10.dp), content = content)
}

@Composable
private fun PrivilegeNote(session: DesktopSession) {
    val b = LocalBreeze.current
    val shz by session.app.shizuku.state.collectAsState()
    val rootOk by session.app.root.available.collectAsState()
    val name = session.app.controls.backendName()
    val tag = "${shz.ordinal}/${rootOk}"
    key(tag) {
        if (name != null) T("Controle avançado ativo: $name.", 11.sp, Color(0xFF27AE60), modifier = Modifier.padding(top = 6.dp))
        else T("Sem Shizuku/root: o que o Android bloqueia abre o painel do sistema. Conecte o Shizuku em Integração Android para controlar direto.", 11.sp, b.dim, modifier = Modifier.padding(top = 6.dp))
    }
}

@Composable
private fun SoftField(value: String, hint: String, secret: Boolean, onChange: (String) -> Unit) {
    val b = LocalBreeze.current
    Box(Modifier.fillMaxWidth().clip(RoundedCornerShape(5.dp)).background(b.view).border(1.dp, b.line, RoundedCornerShape(5.dp)).padding(8.dp)) {
        if (value.isEmpty()) T(hint, 12.sp, b.dim)
        BasicTextField(value, onChange, Modifier.fillMaxWidth(), singleLine = true,
            textStyle = TextStyle(color = b.text, fontSize = 13.sp), cursorBrush = SolidColor(b.accent),
            visualTransformation = if (secret) PasswordVisualTransformation() else androidx.compose.ui.text.input.VisualTransformation.None)
    }
}

@Composable
private fun SignalBars(bars: Int, dp: Dp = 16.dp) {
    val b = LocalBreeze.current
    Canvas(Modifier.size(dp)) {
        val w = this.size.width / 5f
        for (i in 0 until 4) {
            val h = this.size.height * (0.3f + 0.7f * (i + 1) / 4f)
            drawRect(if (i < bars) b.text else b.dim.copy(alpha = .35f), Offset(i * w * 1.25f, this.size.height - h), Size(w, h))
        }
    }
}

@Composable
private fun ItemRow(expanded: Boolean, onToggle: () -> Unit, lead: @Composable () -> Unit, title: String, sub: String, trail: @Composable () -> Unit = {},
                    details: @Composable ColumnScope.() -> Unit) {
    val b = LocalBreeze.current
    Column(Modifier.fillMaxWidth().padding(vertical = 1.dp).clip(RoundedCornerShape(6.dp)).background(if (expanded) b.hover else Color.Transparent)) {
        Row(Modifier.fillMaxWidth().breezeClickable(6.dp) { onToggle() }.padding(horizontal = 8.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            lead()
            Spacer(Modifier.width(10.dp))
            Column(Modifier.weight(1f)) {
                T(title, 13.sp, weight = FontWeight.Medium, maxLines = 1)
                if (sub.isNotEmpty()) T(sub, 11.sp, b.dim, maxLines = 1)
            }
            trail()
        }
        if (expanded) Column(Modifier.fillMaxWidth().padding(start = 12.dp, end = 12.dp, bottom = 10.dp), content = details)
    }
}

@Composable
fun NetworkPanel(session: DesktopSession, inSettings: Boolean = false) {
    val ctl = session.app.controls
    val b = LocalBreeze.current
    val st by ctl.wifi.collectAsState()
    val scope = rememberCoroutineScope()
    var openId by remember { mutableStateOf<String?>(null) }
    var pass by remember { mutableStateOf("") }
    var working by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        ctl.refreshWifi()
        if (ctl.wifiEnabledNow()) ctl.scanWifi()
        while (true) { delay(4000); ctl.refreshWifi() }
    }
    fun toast(m: String?) { if (m != null) session.showToast(m) }

    PanelHeader("wifi", "Redes sem fio", st.current?.let { "Conectado a ${it.ssid}" } ?: if (st.enabled) "Não conectado" else "Wi-Fi desligado") {
        IconButton("refresh", enabled = st.enabled && !st.scanning) { scope.launch { ctl.scanWifi() } }
        Spacer(Modifier.width(6.dp))
        BreezeSwitch(st.enabled, { on -> scope.launch { toast(ctl.setWifi(on)) } })
    }

    st.current?.let { c ->
        InfoCard {
            T(c.ssid, 13.sp, weight = FontWeight.SemiBold)
            val bits = buildList {
                if (c.ip.isNotEmpty()) add("IP ${c.ip}")
                if (c.linkMbps > 0) add("${c.linkMbps} Mb/s")
                if (c.freqMhz > 0) add("${c.freqMhz} MHz")
                add("${c.rssi} dBm")
            }
            T(bits.joinToString(" · "), 11.sp, b.dim)
        }
    }

    if (!st.enabled) T("Ligue o Wi-Fi para ver as redes ao alcance.", 12.sp, b.dim, modifier = Modifier.padding(vertical = 8.dp))
    else {
        if (st.needsPermission) {
            InfoCard {
                T("Para listar as redes, o Android exige permissão. Sem Shizuku, o sistema vai pedir a sua autorização.", 12.sp)
                BreezeButton("Conceder permissão", {
                    scope.launch { ctl.ensure(ctl.wifiPerms(), session.requestPermissions); ctl.refreshWifi() }
                }, Modifier.padding(top = 8.dp), primary = true)
            }
        }
        if (st.scanning) T("Procurando redes…", 12.sp, b.dim, modifier = Modifier.padding(vertical = 6.dp))
        SectionTitle("Redes disponíveis")
        if (st.networks.isEmpty() && !st.scanning && !st.needsPermission)
            T("Nenhuma rede encontrada. Toque em atualizar; sem Shizuku e sem a permissão o Android não entrega a lista.", 12.sp, b.dim)
        st.networks.forEach { n ->
            val id = n.ssid
            ItemRow(
                expanded = openId == id, onToggle = { openId = if (openId == id) null else id; pass = "" },
                lead = { SignalBars(n.bars) }, title = n.ssid,
                sub = listOfNotNull(n.security, n.band.ifEmpty { null }, if (n.saved) "salva" else null, if (n.connected) "conectada" else null).joinToString(" · "),
                trail = { if (n.secured) BreezeIcon("lock", 12.dp, b.dim) },
            ) {
                NetworkActions(ctl, n, pass, { pass = it }, working) { action ->
                    scope.launch {
                        working = true
                        toast(action())
                        working = false
                    }
                }
            }
        }
    }
    PrivilegeNote(session)
    Row(Modifier.padding(top = 8.dp)) {
        if (!inSettings) { BreezeButton("Configurar rede…", { session.openTool("settings", "network") }); Spacer(Modifier.width(8.dp)) }
        BreezeButton("Ajustes do Android", { ctl.openAndroidWifiSettings() })
    }
}

@Composable
private fun ColumnScope.NetworkActions(ctl: org.plasma.app.system.SystemControls, n: WifiNet, pass: String, onPass: (String) -> Unit, working: Boolean,
                                       exec: (suspend () -> String?) -> Unit) {
    if (!n.connected && n.secured) SoftField(pass, if (n.saved) "Senha (opcional em redes salvas)" else "Senha", true, onPass)
    Row(Modifier.padding(top = 8.dp)) {
        if (!n.connected) BreezeButton(if (working) "Conectando…" else "Conectar", { exec { ctl.connectWifi(n, pass.ifEmpty { null }) } }, enabled = !working, primary = true)
        if (n.saved || n.connected) {
            if (!n.connected) Spacer(Modifier.width(8.dp))
            BreezeButton("Esquecer rede", { exec { ctl.forgetWifi(n) } }, enabled = !working)
        }
    }
}

@Composable
fun BluetoothPanel(session: DesktopSession, inSettings: Boolean = false) {
    val ctl = session.app.controls
    val b = LocalBreeze.current
    val st by ctl.bluetooth.collectAsState()
    val scope = rememberCoroutineScope()
    var openAddr by remember { mutableStateOf<String?>(null) }
    var working by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        ctl.refreshBluetooth()
        while (true) { delay(4000); ctl.refreshBluetooth() }
    }
    DisposableEffect(Unit) { onDispose { ctl.stopBluetoothScan() } }
    fun toast(m: String?) { if (m != null) session.showToast(m) }

    val sub = when {
        !st.supported -> "Sem Bluetooth neste aparelho"
        !st.enabled -> "Bluetooth desligado"
        st.paired.any { it.connected } -> "Conectado a " + st.paired.filter { it.connected }.joinToString { it.name }
        else -> "Ligado, sem dispositivos conectados"
    }
    PanelHeader("bluetooth", "Bluetooth", sub) {
        if (st.supported) {
            IconButton("refresh", enabled = st.enabled && !st.needsPermission) {
                scope.launch { if (st.discovering) ctl.stopBluetoothScan() else toast(ctl.scanBluetooth()) }
            }
            Spacer(Modifier.width(6.dp))
            BreezeSwitch(st.enabled, { on -> scope.launch { toast(ctl.setBluetooth(on)) } })
        }
    }

    if (st.supported && st.enabled) {
        if (st.needsPermission || !ctl.btPerms().all { ctl.has(it) }) {
            InfoCard {
                T("Para listar, parear e procurar dispositivos o Android exige a permissão de Bluetooth. Com Shizuku ela é concedida sozinha; sem ele, o sistema pede a sua autorização.", 12.sp)
                BreezeButton("Conceder permissão", {
                    scope.launch { ctl.ensure(ctl.btPerms(), session.requestPermissions); ctl.refreshBluetooth() }
                }, Modifier.padding(top = 8.dp), primary = true)
            }
        }
        if (st.discovering) T("Procurando dispositivos próximos…", 12.sp, b.dim, modifier = Modifier.padding(vertical = 6.dp))
        SectionTitle("Dispositivos pareados")
        if (st.paired.isEmpty() && !st.needsPermission) T("Nenhum dispositivo pareado.", 12.sp, b.dim)
        st.paired.forEach { d ->
            ItemRow(
                expanded = openAddr == d.address, onToggle = { openAddr = if (openAddr == d.address) null else d.address },
                lead = { BreezeIcon(d.icon, 18.dp, if (d.connected) b.accent else b.text) },
                title = d.name, sub = if (d.connected) "Conectado" else "Pareado",
            ) {
                Row(Modifier.padding(top = 6.dp)) {
                    BreezeButton(if (d.connected) "Desconectar" else "Conectar", {
                        scope.launch { working = true; toast(ctl.bluetoothAction(if (d.connected) "disconnect" else "connect", d)); working = false }
                    }, enabled = !working, primary = !d.connected)
                    Spacer(Modifier.width(8.dp))
                    BreezeButton("Esquecer", { scope.launch { working = true; toast(ctl.bluetoothAction("unpair", d)); working = false } }, enabled = !working)
                }
                T(d.address, 10.sp, b.dim, modifier = Modifier.padding(top = 6.dp))
            }
        }
        if (st.nearby.isNotEmpty()) {
            SectionTitle("Dispositivos próximos")
            st.nearby.forEach { d ->
                ItemRow(
                    expanded = openAddr == d.address, onToggle = { openAddr = if (openAddr == d.address) null else d.address },
                    lead = { BreezeIcon(d.icon, 18.dp) }, title = d.name, sub = d.rssi?.let { "$it dBm" } ?: "",
                ) {
                    BreezeButton("Parear", { scope.launch { working = true; toast(ctl.pairBluetooth(d)); working = false } }, Modifier.padding(top = 6.dp), enabled = !working, primary = true)
                    T(d.address, 10.sp, b.dim, modifier = Modifier.padding(top = 6.dp))
                }
            }
        }
    }
    PrivilegeNote(session)
    T("Conectar, desconectar e esquecer usam o serviço Shizuku (recurso experimental: depende da versão do Android e do fabricante); sem ele abrimos os ajustes do Android.", 10.sp, b.dim, modifier = Modifier.padding(top = 4.dp))
    Row(Modifier.padding(top = 8.dp)) {
        if (!inSettings) { BreezeButton("Configurar Bluetooth…", { session.openTool("settings", "bluetooth") }); Spacer(Modifier.width(8.dp)) }
        BreezeButton("Ajustes do Android", { ctl.openAndroidBluetoothSettings() })
    }
}

@Composable
fun VolumePanel(session: DesktopSession, inSettings: Boolean = false) {
    val ctl = session.app.controls
    val b = LocalBreeze.current
    val st by ctl.audio.collectAsState()
    val scope = rememberCoroutineScope()
    LaunchedEffect(Unit) { while (true) { withContext(Dispatchers.IO) { ctl.refreshAudio() }; delay(500) } }
    fun toast(m: String?) { if (m != null) session.showToast(m) }

    val music = st.streams.firstOrNull { it.stream == AudioManager.STREAM_MUSIC }
    PanelHeader(if (music?.muted == true) "volumeMute" else "volume", "Volume de áudio",
        music?.let { if (it.muted) "Mudo" else "${it.value * 100 / it.max}%" } ?: "Lendo…") {}

    st.streams.forEach { s ->
        Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(30.dp).breezeClickable(5.dp) { scope.launch { toast(ctl.toggleMute(s.stream)) } }, contentAlignment = Alignment.Center) {
                BreezeIcon(if (s.muted || s.value == 0) "volumeMute" else "volume", 18.dp)
            }
            Spacer(Modifier.width(8.dp))
            Column(Modifier.weight(1f)) {
                Row { T(s.label, 12.sp, modifier = Modifier.weight(1f)); T("${s.value * 100 / s.max}%", 11.sp, b.dim) }
                BreezeSlider(s.value.toFloat(), { f ->
                    val v = Math.round(f)
                    if (v != s.value) scope.launch { toast(ctl.setVolume(s.stream, v)) }
                }, 0f..s.max.toFloat(), Modifier.padding(top = 2.dp), steps = (s.max - 1).coerceAtLeast(0))
            }
        }
    }
    if (st.outputs.isNotEmpty()) {
        SectionTitle("Saídas de áudio")
        st.outputs.forEach { BulletText(it) }
        T("O Android decide a saída ativa; apps comuns não podem trocá-la.", 10.sp, b.dim, modifier = Modifier.padding(top = 4.dp))
    }
    Choice("Modo do toque", listOf(
        AudioManager.RINGER_MODE_NORMAL to "Normal", AudioManager.RINGER_MODE_VIBRATE to "Vibrar", AudioManager.RINGER_MODE_SILENT to "Silencioso",
    ), st.ringerMode) { m -> scope.launch { toast(ctl.setRingerMode(m)) } }
    Row(Modifier.padding(top = 8.dp)) {
        if (!inSettings) { BreezeButton("Configurar som…", { session.openTool("settings", "audio") }); Spacer(Modifier.width(8.dp)) }
        BreezeButton("Ajustes do Android", { ctl.openAndroidSoundSettings() })
    }
}

@Composable
fun BrightnessPanel(session: DesktopSession, inSettings: Boolean = false) {
    val ctl = session.app.controls
    val b = LocalBreeze.current
    val st by ctl.brightness.collectAsState()
    val scope = rememberCoroutineScope()
    var local by remember { mutableStateOf<Int?>(null) }
    var note by remember { mutableStateOf<String?>(null) }
    LaunchedEffect(Unit) { while (true) { withContext(Dispatchers.IO) { ctl.refreshBrightness() }; delay(1000) } }
    fun toast(m: String?) { if (m != null) session.showToast(m) }

    val shown = local ?: st.value
    PanelHeader("brightness", "Brilho e cor", "${shown * 100 / 255}%") {}
    SliderRow("Brilho da tela", (shown / 255f).coerceIn(0f, 1f), 0f..1f, "${shown * 100 / 255}%") { f ->
        val v = (f * 254f).toInt() + 1
        local = v
        scope.launch {
            when (ctl.setBrightness(v)) {
                BrightnessResult.NEEDS_PERMISSION -> {
                    session.setWindowBrightness(v / 255f)
                    note = "Sem permissão de sistema: o brilho foi ajustado só dentro do desktop."
                }
                else -> { session.setWindowBrightness(null); note = null; local = null }
            }
        }
    }
    SwitchRow("Brilho automático", st.auto, "Deixa o Android ajustar o brilho pela luz ambiente.") { on ->
        scope.launch { toast(ctl.setAutoBrightness(on)) }
    }
    SwitchRow("Luz noturna", st.nightLight, "Reduz a luz azul. Exige Shizuku/root e depende do fabricante (AOSP e One UI).") { on ->
        scope.launch { toast(ctl.setNightLight(on)) }
    }
    if (!st.canWrite) {
        InfoCard {
            T(note ?: "Para mudar o brilho do sistema inteiro sem Shizuku, o Android exige a permissão “Modificar configurações do sistema”.", 12.sp)
            BreezeButton("Conceder permissão", { scope.launch { ctl.ensureWriteSettings() } }, Modifier.padding(top = 8.dp), primary = true)
        }
    }
    PrivilegeNote(session)
    Row(Modifier.padding(top = 8.dp)) {
        if (!inSettings) { BreezeButton("Configurar brilho…", { session.openTool("settings", "brightness") }); Spacer(Modifier.width(8.dp)) }
        BreezeButton("Ajustes do Android", { ctl.openAndroidDisplaySettings() })
    }
}
