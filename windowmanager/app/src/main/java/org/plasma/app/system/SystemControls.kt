
package org.plasma.app.system

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothClass
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.net.ConnectivityManager
import android.net.Uri
import android.net.wifi.ScanResult
import android.net.wifi.SupplicantState
import android.net.wifi.WifiManager
import android.os.Build
import android.provider.Settings
import androidx.core.content.ContextCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.plasma.app.PlasmaApp
import org.plasma.bridge.PrivilegedBackend
import org.plasma.core.PlasmaLog
import java.net.Inet4Address
import kotlin.coroutines.resume

data class WifiNet(
    val ssid: String,
    val bssid: String = "",
    val level: Int = -100,
    val security: String = "Aberta",
    val freqMhz: Int = 0,
    val connected: Boolean = false,
    val saved: Boolean = false,
    val networkId: Int = -1,
) {
    val secured: Boolean get() = security != "Aberta"
    val bars: Int get() = when { level >= -55 -> 4; level >= -67 -> 3; level >= -78 -> 2; level >= -90 -> 1; else -> 0 }
    val band: String get() = when { freqMhz >= 5925 -> "6 GHz"; freqMhz >= 4900 -> "5 GHz"; freqMhz >= 2400 -> "2,4 GHz"; else -> "" }
}

data class WifiDetails(val ssid: String, val ip: String, val linkMbps: Int, val freqMhz: Int, val rssi: Int, val bssid: String)

data class WifiState(
    val enabled: Boolean = false,
    val networks: List<WifiNet> = emptyList(),
    val current: WifiDetails? = null,
    val scanning: Boolean = false,
    val needsPermission: Boolean = false,
    val source: String = "",
)

data class BtDev(
    val name: String,
    val address: String,
    val paired: Boolean,
    val connected: Boolean = false,
    val major: Int = BluetoothClass.Device.Major.UNCATEGORIZED,
    val rssi: Int? = null,
) {
    val icon: String get() = when (major) {
        BluetoothClass.Device.Major.AUDIO_VIDEO -> "headphones"
        BluetoothClass.Device.Major.PHONE -> "phone"
        BluetoothClass.Device.Major.COMPUTER -> "monitor"
        BluetoothClass.Device.Major.PERIPHERAL -> "keyboard"
        else -> "bluetooth"
    }
}

data class BtState(
    val supported: Boolean = true,
    val enabled: Boolean = false,
    val paired: List<BtDev> = emptyList(),
    val nearby: List<BtDev> = emptyList(),
    val discovering: Boolean = false,
    val needsPermission: Boolean = false,
)

data class AudioStreamInfo(val stream: Int, val label: String, val value: Int, val max: Int, val muted: Boolean)
data class AudioState(val streams: List<AudioStreamInfo> = emptyList(), val outputs: List<String> = emptyList(), val ringerMode: Int = AudioManager.RINGER_MODE_NORMAL)

data class BrightnessState(val value: Int = 128, val auto: Boolean = false, val canWrite: Boolean = false, val nightLight: Boolean = false)

data class QuickState(
    val wifiOn: Boolean = false, val wifiSsid: String? = null, val btOn: Boolean = false,
    val volumePct: Int = 0, val muted: Boolean = false, val brightnessPct: Int = 0,
)

enum class BrightnessResult { SYSTEM, SHELL, NEEDS_PERMISSION }

class SystemControls(private val app: PlasmaApp) {
    private val ctx: Context get() = app

    private val _wifi = MutableStateFlow(WifiState())
    val wifi: StateFlow<WifiState> = _wifi.asStateFlow()
    private val _bt = MutableStateFlow(BtState())
    val bluetooth: StateFlow<BtState> = _bt.asStateFlow()
    private val _audio = MutableStateFlow(AudioState())
    val audio: StateFlow<AudioState> = _audio.asStateFlow()
    private val _bright = MutableStateFlow(BrightnessState())
    val brightness: StateFlow<BrightnessState> = _bright.asStateFlow()

    private val _version = MutableStateFlow(0)
    val version: StateFlow<Int> = _version.asStateFlow()
    fun bump() { _version.update { it + 1 } }

    private val found = LinkedHashMap<String, BtDev>()
    private var btReceiverOn = false

    fun backend(): PrivilegedBackend? = app.backends.current()
    fun backendName(): String? = backend()?.name

    fun has(perm: String): Boolean = ctx.checkSelfPermission(perm) == PackageManager.PERMISSION_GRANTED

    fun wifiPerms(): List<String> =
        if (Build.VERSION.SDK_INT >= 33) listOf(Manifest.permission.NEARBY_WIFI_DEVICES) else listOf(Manifest.permission.ACCESS_FINE_LOCATION)

    fun btPerms(): List<String> =
        if (Build.VERSION.SDK_INT >= 31) listOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN)
        else listOf(Manifest.permission.ACCESS_FINE_LOCATION)

    private fun grantViaShell(perm: String): Boolean {
        val be = backend() ?: return false
        val out = be.exec(listOf("pm", "grant", ctx.packageName, perm))
        PlasmaLog.i("sysctl", "pm grant $perm -> ${out.replace('\n', ' ').take(120)}")
        return has(perm)
    }

    suspend fun ensure(perms: List<String>, requester: (Array<String>) -> Unit): Boolean = withContext(Dispatchers.IO) {
        val missing = perms.filter { !has(it) }
        if (missing.isEmpty()) return@withContext true
        missing.forEach { grantViaShell(it) }
        val still = perms.filter { !has(it) }
        if (still.isNotEmpty()) withContext(Dispatchers.Main) { requester(still.toTypedArray()) }
        bump()
        still.isEmpty()
    }

    fun canWriteSettings(): Boolean = Settings.System.canWrite(ctx)

    suspend fun ensureWriteSettings(): Boolean = withContext(Dispatchers.IO) {
        if (canWriteSettings()) return@withContext true
        backend()?.exec(listOf("appops", "set", ctx.packageName, "WRITE_SETTINGS", "allow"))
        if (canWriteSettings()) { bump(); return@withContext true }
        open(Intent(Settings.ACTION_MANAGE_WRITE_SETTINGS, Uri.parse("package:${ctx.packageName}")))
        false
    }

    private fun open(i: Intent) {
        runCatching { ctx.startActivity(i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
            .onFailure { PlasmaLog.w("sysctl", "não foi possível abrir ${i.action}", it) }
    }

    fun openAndroidWifiSettings() = open(Intent(Settings.ACTION_WIFI_SETTINGS))
    fun openAndroidBluetoothSettings() = open(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
    fun openAndroidSoundSettings() = open(Intent(Settings.ACTION_SOUND_SETTINGS))
    fun openAndroidDisplaySettings() = open(Intent(Settings.ACTION_DISPLAY_SETTINGS))
    private fun openWifiPanel() = open(Intent(Settings.Panel.ACTION_INTERNET_CONNECTIVITY))

    private fun shellOk(out: String): Boolean = when {
        out.startsWith("exit=") -> out.startsWith("exit=0") && !out.contains("Exception")
        else -> !out.startsWith("erro") && !out.startsWith("timeout") && !out.contains("Exception")
    }
    private fun shellBody(out: String): String = if (out.startsWith("exit=")) out.substringAfter('\n', "") else out

    private val wm: WifiManager get() = ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager

    fun wifiEnabledNow(): Boolean = runCatching { wm.isWifiEnabled }.getOrDefault(false)

    suspend fun setWifi(on: Boolean): String? = withContext(Dispatchers.IO) {
        val be = backend()
        val msg: String? = if (be != null) {
            val out = be.exec(listOf("svc", "wifi", if (on) "enable" else "disable"))
            if (shellOk(out)) null else "Falha ao ${if (on) "ligar" else "desligar"} o Wi-Fi: ${shellBody(out).trim().take(120)}"
        } else {

            openWifiPanel()
            "Sem Shizuku/root: abri o painel de Wi-Fi do Android."
        }
        delay(800)
        refreshWifi()
        bump()
        msg
    }

    @Suppress("DEPRECATION")
    private fun startScanCompat() { runCatching { wm.startScan() } }

    suspend fun scanWifi() {
        _wifi.update { it.copy(scanning = true) }
        withContext(Dispatchers.IO) {
            startScanCompat()
            backend()?.exec(listOf("cmd", "wifi", "start-scan"))
            delay(2500)
        }
        refreshWifi()
        _wifi.update { it.copy(scanning = false) }
    }

    suspend fun refreshWifi() = withContext(Dispatchers.IO) {
        val enabled = wifiEnabledNow()
        val be = backend()
        val hasPerm = wifiPerms().all { has(it) }
        val saved: Map<String, Int> = if (be != null && enabled) savedNetworks(be) else emptyMap()
        var list: List<WifiNet> = emptyList()
        var source = ""
        if (enabled) {
            if (hasPerm) { list = nativeScan(); if (list.isNotEmpty()) source = "API do Android" }
            if (list.isEmpty() && be != null) { list = shellScan(be); if (list.isNotEmpty()) source = be.name }
        }
        val cur = if (enabled) currentWifi(be) else null
        val merged = list.map { n ->
            n.copy(connected = cur != null && cur.ssid == n.ssid, saved = saved.containsKey(n.ssid), networkId = saved[n.ssid] ?: -1)
        }.sortedWith(compareByDescending<WifiNet> { it.connected }.thenByDescending { it.saved }.thenByDescending { it.level })
        _wifi.value = WifiState(
            enabled = enabled, networks = merged, current = cur, scanning = _wifi.value.scanning,
            needsPermission = enabled && !hasPerm && be == null && merged.isEmpty(), source = source,
        )
    }

    private fun security(flags: String): String {
        val f = flags.uppercase()
        return when {
            f.contains("SAE") && f.contains("PSK") -> "WPA2/WPA3"
            f.contains("SAE") -> "WPA3"
            f.contains("EAP") -> "802.1x"
            f.contains("PSK") && f.contains("WPA2") -> "WPA2"
            f.contains("PSK") -> "WPA"
            f.contains("WEP") -> "WEP"
            f.contains("OWE") -> "OWE"
            else -> "Aberta"
        }
    }

    @SuppressLint("MissingPermission")
    private fun nativeScan(): List<WifiNet> = try {
        wm.scanResults.orEmpty().mapNotNull { r: ScanResult ->
            @Suppress("DEPRECATION") val ssid = r.SSID.orEmpty()
            if (ssid.isEmpty()) null else WifiNet(ssid, r.BSSID.orEmpty(), r.level, security(r.capabilities.orEmpty()), r.frequency)
        }.groupBy { it.ssid }.map { (_, v) -> v.maxBy { it.level } }
    } catch (_: SecurityException) { emptyList() }

    private val scanLine = Regex("""^\s*((?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2})\s+(\d+)\s+(-?\d+)\S*\s+(\S+)\s+(.*)$""")
    private val ssidFlags = Regex("""^(.*?)\s*((?:\[[^\]]*\])*)\s*$""")

    private fun shellScan(be: PrivilegedBackend): List<WifiNet> {
        val out = shellBody(be.exec(listOf("cmd", "wifi", "list-scan-results")))
        return out.lineSequence().mapNotNull { line ->
            val m = scanLine.find(line) ?: return@mapNotNull null
            val rest = ssidFlags.find(m.groupValues[5].trim()) ?: return@mapNotNull null
            val ssid = rest.groupValues[1].trim()
            if (ssid.isEmpty()) null
            else WifiNet(ssid, m.groupValues[1], m.groupValues[3].toIntOrNull() ?: -100, security(rest.groupValues[2]), m.groupValues[2].toIntOrNull() ?: 0)
        }.toList().groupBy { it.ssid }.map { (_, v) -> v.maxBy { it.level } }
    }

    private val savedLine = Regex("""^\s*(\d+)\s+(.+?)\s{2,}(\S+)\s*$""")

    private fun savedNetworks(be: PrivilegedBackend): Map<String, Int> {
        val out = shellBody(be.exec(listOf("cmd", "wifi", "list-networks")))
        val map = HashMap<String, Int>()
        out.lineSequence().forEach { l -> savedLine.find(l)?.let { map[it.groupValues[2].trim()] = it.groupValues[1].toInt() } }
        return map
    }

    @Suppress("DEPRECATION")
    private fun currentWifi(be: PrivilegedBackend?): WifiDetails? {
        val info = runCatching { wm.connectionInfo }.getOrNull() ?: return null
        if (info.supplicantState != SupplicantState.COMPLETED) return null
        var ssid = info.ssid?.trim('"')?.takeIf { it.isNotEmpty() && it != "<unknown ssid>" }
        if (ssid == null && be != null) {
            val st = shellBody(be.exec(listOf("cmd", "wifi", "status")))
            ssid = Regex("""connected to "(.*?)"""").find(st)?.groupValues?.get(1)
                ?: Regex("""SSID: "(.*?)"""").find(st)?.groupValues?.get(1)
        }
        val ip = runCatching {
            val cm = ctx.getSystemService(ConnectivityManager::class.java)
            cm.getLinkProperties(cm.activeNetwork)?.linkAddresses?.map { it.address }?.firstOrNull { it is Inet4Address }?.hostAddress
        }.getOrNull().orEmpty()
        return WifiDetails(ssid ?: "Rede conectada", ip, info.linkSpeed, info.frequency, info.rssi, info.bssid.orEmpty())
    }

    suspend fun connectWifi(n: WifiNet, password: String?): String? = withContext(Dispatchers.IO) {
        val be = backend()
        if (be == null || Build.VERSION.SDK_INT < 30) {
            openWifiPanel()
            return@withContext if (be == null) "Sem Shizuku/root: abri o painel de Wi-Fi do Android para conectar a “${n.ssid}”."
            else "Android 10 não tem `cmd wifi connect-network`: abri o painel do sistema."
        }
        val sec = when (n.security) {
            "Aberta" -> "open"
            "OWE" -> "owe"
            "WPA3" -> "wpa3"
            "WPA", "WPA2", "WPA2/WPA3" -> "wpa2"
            else -> {
                openWifiPanel()
                return@withContext "Redes ${n.security} não podem ser configuradas por comando: abri o painel do sistema."
            }
        }
        if (sec != "open" && sec != "owe" && password.isNullOrEmpty()) {
            return@withContext if (n.saved) "Rede salva: o Android reconecta sozinho quando ela está ao alcance. Informe a senha para forçar a conexão agora."
            else "Informe a senha da rede."
        }
        val cmd = buildList { addAll(listOf("cmd", "wifi", "connect-network", n.ssid, sec)); if (sec != "open" && sec != "owe") add(password!!) }
        val out = be.exec(cmd, 12_000)
        if (!shellOk(out)) return@withContext "Falha ao conectar: ${shellBody(out).trim().take(140)}"
        delay(3500)
        refreshWifi(); bump()
        if (_wifi.value.current?.ssid == n.ssid) null
        else "O comando foi aceito, mas a conexão não foi confirmada (senha incorreta ou sinal fraco)."
    }

    suspend fun forgetWifi(n: WifiNet): String? = withContext(Dispatchers.IO) {
        val be = backend()
        if (be == null || n.networkId < 0) {
            openWifiPanel()
            return@withContext "Abri o painel do sistema para esquecer “${n.ssid}”."
        }
        val out = be.exec(listOf("cmd", "wifi", "forget-network", n.networkId.toString()))
        delay(800); refreshWifi(); bump()
        if (shellOk(out)) null else "Falha ao esquecer a rede: ${shellBody(out).trim().take(120)}"
    }

    private val adapter: BluetoothAdapter? get() = ctx.getSystemService(BluetoothManager::class.java)?.adapter

    fun btEnabledNow(): Boolean = runCatching { adapter?.isEnabled == true }.getOrDefault(false)

    suspend fun setBluetooth(on: Boolean): String? = withContext(Dispatchers.IO) {
        if (adapter == null) return@withContext "Este aparelho não tem Bluetooth."
        val be = backend()
        val msg: String? = if (be != null) {
            var out = be.exec(listOf("svc", "bluetooth", if (on) "enable" else "disable"))
            if (!shellOk(out)) out = be.exec(listOf("cmd", "bluetooth_manager", if (on) "enable" else "disable"))
            if (shellOk(out)) null else "Falha ao ${if (on) "ligar" else "desligar"} o Bluetooth: ${shellBody(out).trim().take(120)}"
        } else {

            if (on) { open(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)); "Sem Shizuku/root: o Android pediu a sua confirmação para ligar o Bluetooth." }
            else { openAndroidBluetoothSettings(); "Sem Shizuku/root: abri os ajustes de Bluetooth do Android para desligar." }
        }
        delay(900)
        refreshBluetooth()
        bump()
        msg
    }

    @SuppressLint("MissingPermission")
    suspend fun refreshBluetooth() = withContext(Dispatchers.IO) {
        val a = adapter
        if (a == null) { _bt.value = BtState(supported = false); return@withContext }
        val enabled = runCatching { a.isEnabled }.getOrDefault(false)
        val hasConnect = if (Build.VERSION.SDK_INT >= 31) has(Manifest.permission.BLUETOOTH_CONNECT) else true
        if (!enabled) { _bt.value = BtState(enabled = false, needsPermission = false); return@withContext }
        if (!hasConnect) { _bt.value = BtState(enabled = true, needsPermission = true, discovering = _bt.value.discovering); return@withContext }
        val connected = connectedAddresses(a)
        val paired = runCatching { a.bondedDevices.orEmpty() }.getOrDefault(emptySet()).map { toDev(it, true, connected) }
            .sortedWith(compareByDescending<BtDev> { it.connected }.thenBy { it.name.lowercase() })
        val pairedAddr = paired.map { it.address }.toSet()
        val nearby = synchronized(found) { found.values.filter { it.address !in pairedAddr }.sortedByDescending { it.rssi ?: -127 } }
        _bt.value = BtState(enabled = true, paired = paired, nearby = nearby, discovering = _bt.value.discovering)
    }

    @SuppressLint("MissingPermission")
    private fun toDev(d: BluetoothDevice, paired: Boolean, connected: Set<String>, rssi: Int? = null): BtDev {
        val name = runCatching { d.name ?: (if (Build.VERSION.SDK_INT >= 30) d.alias else null) }.getOrNull()
        val major = runCatching { d.bluetoothClass?.majorDeviceClass }.getOrNull() ?: BluetoothClass.Device.Major.UNCATEGORIZED
        return BtDev(name?.takeIf { it.isNotBlank() } ?: d.address, d.address, paired, d.address in connected, major, rssi)
    }

    private suspend fun connectedAddresses(a: BluetoothAdapter): Set<String> {
        val all = HashSet<String>()
        val profiles = buildList {
            add(BluetoothProfile.A2DP); add(BluetoothProfile.HEADSET)
            if (Build.VERSION.SDK_INT >= 29) add(BluetoothProfile.HEARING_AID)
        }
        profiles.forEach { all += profileConnected(a, it) }
        return all
    }

    @SuppressLint("MissingPermission")
    private suspend fun profileConnected(a: BluetoothAdapter, profile: Int): Set<String> = withTimeoutOrNull(1800) {
        suspendCancellableCoroutine<Set<String>> { c ->
            val l = object : BluetoothProfile.ServiceListener {
                override fun onServiceConnected(p: Int, proxy: BluetoothProfile) {
                    val r = runCatching { proxy.connectedDevices.map { it.address }.toSet() }.getOrDefault(emptySet())
                    runCatching { a.closeProfileProxy(p, proxy) }
                    if (c.isActive) c.resume(r)
                }
                override fun onServiceDisconnected(p: Int) {}
            }
            val started = runCatching { a.getProfileProxy(ctx, l, profile) }.getOrDefault(false)
            if (!started && c.isActive) c.resume(emptySet())
        }
    } ?: emptySet()

    private val btReceiver = object : BroadcastReceiver() {
        @SuppressLint("MissingPermission")
        override fun onReceive(c: Context, i: Intent) {
            when (i.action) {
                BluetoothDevice.ACTION_FOUND -> {
                    val dev = if (Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                    else @Suppress("DEPRECATION") i.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                    if (dev != null) {
                        val rssi = i.getShortExtra(BluetoothDevice.EXTRA_RSSI, Short.MIN_VALUE).toInt().takeIf { it != Short.MIN_VALUE.toInt() }
                        synchronized(found) { found[dev.address] = toDev(dev, false, emptySet(), rssi) }
                        _bt.update { it.copy(nearby = synchronized(found) { found.values.filter { d -> it.paired.none { p -> p.address == d.address } }.sortedByDescending { d -> d.rssi ?: -127 } }) }
                    }
                }
                BluetoothAdapter.ACTION_DISCOVERY_FINISHED -> _bt.update { it.copy(discovering = false) }
                BluetoothDevice.ACTION_BOND_STATE_CHANGED -> app.appScope.launch { refreshBluetooth(); bump() }
            }
        }
    }

    private fun registerBtReceiver() {
        if (btReceiverOn) return
        val f = IntentFilter().apply {
            addAction(BluetoothDevice.ACTION_FOUND); addAction(BluetoothAdapter.ACTION_DISCOVERY_FINISHED)
            addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
        }
        ContextCompat.registerReceiver(ctx, btReceiver, f, ContextCompat.RECEIVER_EXPORTED)
        btReceiverOn = true
    }

    @SuppressLint("MissingPermission")
    suspend fun scanBluetooth(): String? = withContext(Dispatchers.IO) {
        val a = adapter ?: return@withContext "Este aparelho não tem Bluetooth."
        if (!a.isEnabled) return@withContext "Ligue o Bluetooth primeiro."
        if (!btPerms().all { has(it) }) return@withContext "Falta a permissão de Bluetooth."
        registerBtReceiver()
        synchronized(found) { found.clear() }
        try {
            if (a.isDiscovering) a.cancelDiscovery()
            if (!a.startDiscovery()) return@withContext "O Android recusou iniciar a busca (tente de novo em alguns segundos)."
        } catch (t: SecurityException) { return@withContext "Permissão de Bluetooth negada: ${t.message}" }
        _bt.update { it.copy(discovering = true) }
        null
    }

    @SuppressLint("MissingPermission")
    fun stopBluetoothScan() { runCatching { adapter?.cancelDiscovery() }; _bt.update { it.copy(discovering = false) } }

    @SuppressLint("MissingPermission")
    suspend fun pairBluetooth(d: BtDev): String? = withContext(Dispatchers.IO) {
        val a = adapter ?: return@withContext "Este aparelho não tem Bluetooth."
        try {
            runCatching { a.cancelDiscovery() }
            if (!a.getRemoteDevice(d.address).createBond()) "Não foi possível iniciar o pareamento com ${d.name}."
            else "Pareamento iniciado: confirme o código no diálogo do Android, se ele aparecer."
        } catch (t: SecurityException) { "Permissão de Bluetooth negada: ${t.message}" }
    }

    suspend fun bluetoothAction(op: String, d: BtDev): String? = withContext(Dispatchers.IO) {
        val be = backend()
        if (be == null) {
            openAndroidBluetoothSettings()
            return@withContext "Sem Shizuku/root: abri os ajustes de Bluetooth do Android para ${when (op) { "connect" -> "conectar"; "disconnect" -> "desconectar"; else -> "esquecer" }} ${d.name}."
        }
        val out = be.bluetooth(op, d.address)
        delay(900); refreshBluetooth(); bump()
        if (out.startsWith("ok")) null else "Não deu certo (${op}): ${out.removePrefix("erro: ").take(150)}"
    }

    private val am: AudioManager get() = ctx.getSystemService(AudioManager::class.java)

    private val streamDefs = listOf(
        AudioManager.STREAM_MUSIC to "Mídia e vídeo",
        AudioManager.STREAM_RING to "Toque",
        AudioManager.STREAM_NOTIFICATION to "Notificações",
        AudioManager.STREAM_ALARM to "Alarme",
    )

    fun refreshAudio() {
        val a = am
        val streams = streamDefs.map { (s, label) ->
            AudioStreamInfo(s, label, a.getStreamVolume(s), a.getStreamMaxVolume(s).coerceAtLeast(1), a.isStreamMute(s))
        }
        val outs = runCatching {
            a.getDevices(AudioManager.GET_DEVICES_OUTPUTS).map { outputLabel(it) }.distinct()
        }.getOrDefault(emptyList())
        _audio.value = AudioState(streams, outs, a.ringerMode)
    }

    private fun outputLabel(d: AudioDeviceInfo): String {
        val product = d.productName?.toString().orEmpty()
        return when (d.type) {
            AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Alto-falante do aparelho"
            AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "Auricular (chamadas)"
            AudioDeviceInfo.TYPE_WIRED_HEADPHONES, AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Fone com fio"
            AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "Bluetooth (mídia)${if (product.isNotBlank()) " · $product" else ""}"
            AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "Bluetooth (chamadas)${if (product.isNotBlank()) " · $product" else ""}"
            AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET, AudioDeviceInfo.TYPE_USB_ACCESSORY -> "USB${if (product.isNotBlank()) " · $product" else ""}"
            AudioDeviceInfo.TYPE_HDMI, AudioDeviceInfo.TYPE_HDMI_ARC -> "HDMI"
            else -> product.ifBlank { "Saída de áudio (tipo ${d.type})" }
        }
    }

    suspend fun setVolume(stream: Int, value: Int): String? = withContext(Dispatchers.IO) {
        val a = am
        val v = value.coerceIn(0, a.getStreamMaxVolume(stream))
        var denied = false
        try { a.setStreamVolume(stream, v, 0) } catch (_: SecurityException) { denied = true }
        if (denied || a.getStreamVolume(stream) != v) {
            val be = backend()
            if (be != null) be.exec(listOf("cmd", "media_session", "volume", "--stream", stream.toString(), "--set", v.toString()))
            else if (denied) return@withContext "O Android bloqueou o ajuste (modo Não Perturbe?). Sem Shizuku/root não há como contornar."
        }
        refreshAudio(); bump()
        null
    }

    suspend fun toggleMute(stream: Int): String? = withContext(Dispatchers.IO) {
        val a = am
        try {
            a.adjustStreamVolume(stream, if (a.isStreamMute(stream)) AudioManager.ADJUST_UNMUTE else AudioManager.ADJUST_MUTE, 0)
        } catch (t: SecurityException) {
            return@withContext "O Android bloqueou silenciar este canal (política de Não Perturbe)."
        }
        refreshAudio(); bump()
        null
    }

    suspend fun setRingerMode(mode: Int): String? = withContext(Dispatchers.IO) {
        try { am.ringerMode = mode } catch (t: SecurityException) {
            return@withContext "Silenciar o toque exige o acesso a Não Perturbe nas configurações do Android."
        }
        refreshAudio(); bump()
        null
    }

    private val cr get() = ctx.contentResolver

    fun refreshBrightness() {
        _bright.value = BrightnessState(
            value = runCatching { Settings.System.getInt(cr, Settings.System.SCREEN_BRIGHTNESS) }.getOrDefault(128),
            auto = runCatching { Settings.System.getInt(cr, Settings.System.SCREEN_BRIGHTNESS_MODE) == Settings.System.SCREEN_BRIGHTNESS_MODE_AUTOMATIC }.getOrDefault(false),
            canWrite = canWriteSettings(),
            nightLight = runCatching {
                Settings.Secure.getInt(cr, "night_display_activated", 0) == 1 || Settings.System.getInt(cr, "blue_light_filter", 0) == 1
            }.getOrDefault(false),
        )
    }

    suspend fun setBrightness(value: Int): BrightnessResult = withContext(Dispatchers.IO) {
        val v = value.coerceIn(1, 255)
        val r = when {
            canWriteSettings() -> {
                Settings.System.putInt(cr, Settings.System.SCREEN_BRIGHTNESS, v); BrightnessResult.SYSTEM
            }
            backend() != null -> {
                backend()!!.exec(listOf("settings", "put", "system", "screen_brightness", v.toString())); BrightnessResult.SHELL
            }
            else -> BrightnessResult.NEEDS_PERMISSION
        }
        refreshBrightness(); bump()
        r
    }

    suspend fun setAutoBrightness(on: Boolean): String? = withContext(Dispatchers.IO) {
        val mode = if (on) Settings.System.SCREEN_BRIGHTNESS_MODE_AUTOMATIC else Settings.System.SCREEN_BRIGHTNESS_MODE_MANUAL
        val msg: String? = when {
            canWriteSettings() -> { Settings.System.putInt(cr, Settings.System.SCREEN_BRIGHTNESS_MODE, mode); null }
            backend() != null -> {
                val out = backend()!!.exec(listOf("settings", "put", "system", "screen_brightness_mode", mode.toString()))
                if (shellOk(out)) null else "Falha: ${shellBody(out).trim().take(100)}"
            }
            else -> "Precisa da permissão “Modificar configurações do sistema” ou de Shizuku/root."
        }
        refreshBrightness(); bump()
        msg
    }

    suspend fun setNightLight(on: Boolean): String? = withContext(Dispatchers.IO) {
        val be = backend() ?: return@withContext "A luz noturna precisa de Shizuku/root (WRITE_SECURE_SETTINGS)."
        val v = if (on) "1" else "0"
        be.exec(listOf("settings", "put", "secure", "night_display_activated", v))
        be.exec(listOf("settings", "put", "system", "blue_light_filter", v))
        refreshBrightness(); bump()
        null
    }

    fun quick(): QuickState {
        refreshAudio(); refreshBrightness()
        val m = _audio.value.streams.firstOrNull { it.stream == AudioManager.STREAM_MUSIC }
        return QuickState(
            wifiOn = wifiEnabledNow(), wifiSsid = _wifi.value.current?.ssid, btOn = btEnabledNow(),
            volumePct = m?.let { it.value * 100 / it.max } ?: 0, muted = m?.muted ?: false,
            brightnessPct = _bright.value.value * 100 / 255,
        )
    }
}
