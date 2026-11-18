
package org.plasma.app.session

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.os.FileObserver
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.plasma.app.PlasmaApp
import org.plasma.core.Keys
import org.plasma.proot.ProotLauncher
import java.io.File
import java.util.Locale

data class LinuxApp(
    val id: String,
    val name: String,
    val comment: String,
    val exec: String,
    val icon: String,
    val categories: List<String>,
    val terminal: Boolean,
)

fun linuxKickoffCategory(app: LinuxApp): String {
    val c = app.categories.map { it.lowercase() }.toSet()
    return when {
        c.any { it in setOf("development", "ide", "debugger", "building", "texteditor") } -> "Desenvolvimento"
        c.any { it in setOf("game", "kgame", "emulator") } -> "Jogos"
        c.any { it in setOf("graphics", "2dgraphics", "3dgraphics", "photography", "rastergraphics", "vectorgraphics") } -> "Gráficos"
        c.any { it in setOf("network", "webbrowser", "email", "instantmessaging", "chat", "filetransfer", "p2p") } -> "Internet"
        c.any { it in setOf("audiovideo", "audio", "video", "player", "recorder", "music", "videoediting") } -> "Multimídia"
        c.any { it in setOf("office", "wordprocessor", "spreadsheet", "presentation", "calendar", "pdf", "viewer") } -> "Escritório"
        c.any { it in setOf("system", "settings", "monitor", "packagemanager", "terminalemulator") } -> "Sistema"
        c.any { it == "help" } -> "Ajuda"
        else -> "Utilitários"
    }
}

object LinuxAppIndex {
    @Volatile var byId: Map<String, LinuxApp> = emptyMap()
}

class LinuxAppCatalog(private val app: PlasmaApp) {

    private val _apps = MutableStateFlow<List<LinuxApp>>(emptyList())
    val apps: StateFlow<List<LinuxApp>> = _apps.asStateFlow()

    private val rootfs: File get() = app.debian.paths.rootfs

    @Volatile private var signature = ""
    @Volatile private var pending: Job? = null
    private val observers = ArrayList<FileObserver>()
    private val observed = HashSet<String>()

    private val iconFiles = java.util.concurrent.ConcurrentHashMap<String, File>()
    private val iconBitmaps = HashMap<String, ImageBitmap>()

    private val builtin = setOf(
        "org.kde.konsole", "konsole", "org.kde.dolphin", "dolphin",
        "systemsettings", "org.kde.systemsettings", "kdesystemsettings",
    )

    private fun appDirs(): List<File> {
        val list = ArrayList<File>()
        list += File(rootfs, "usr/share/applications")
        list += File(rootfs, "usr/local/share/applications")
        list += File(rootfs, "var/lib/flatpak/exports/share/applications")
        list += File(rootfs, "root/.local/share/applications")
        File(rootfs, "home").listFiles()?.filter { it.isDirectory }?.forEach {
            list += File(it, ".local/share/applications")
        }
        return list
    }

    fun start() {
        requestRefresh(0)
    }

    fun requestRefresh(delayMs: Long = 1200) {
        pending?.cancel()
        pending = app.appScope.launch(kotlinx.coroutines.Dispatchers.IO) {
            if (delayMs > 0) delay(delayMs)
            runCatching { refreshNow() }
        }
    }

    private fun watchDirs() {
        for (d in appDirs()) {
            if (!d.isDirectory || !observed.add(d.path)) continue
            @Suppress("DEPRECATION")
            val o = object : FileObserver(
                d.path,
                CREATE or DELETE or MOVED_FROM or MOVED_TO or CLOSE_WRITE,
            ) {
                override fun onEvent(event: Int, path: String?) {
                    if (path != null && path.endsWith(".desktop")) requestRefresh()
                }
            }
            o.startWatching()
            observers += o
        }
    }

    @Synchronized
    private fun refreshNow() {
        if (!app.debian.installed) { LinuxAppIndex.byId = emptyMap(); _apps.value = emptyList(); return }
        watchDirs()

        val dirs = appDirs().filter { it.isDirectory }
        val sig = dirs.joinToString("|") { d ->
            d.path + ":" + (d.listFiles { f -> f.name.endsWith(".desktop") }
                ?.joinToString(",") { it.name + "@" + it.lastModified() + "/" + it.length() } ?: "")
        }
        if (sig == signature) return
        signature = sig

        val byId = LinkedHashMap<String, LinuxApp>()
        for (d in dirs) {
            val files = d.listFiles { f -> f.isFile && f.name.endsWith(".desktop") } ?: continue
            for (f in files.sortedBy { it.name }) {
                val parsed = parse(f) ?: continue
                if (parsed.id in builtin) continue
                byId[parsed.id] = parsed
            }
        }
        val list = byId.values.sortedBy { it.name.lowercase() }

        synchronized(iconBitmaps) { iconBitmaps.clear() }
        resolveIcons(list)
        LinuxAppIndex.byId = list.associateBy { "linux:" + it.id }
        _apps.value = list
    }


    private fun parse(file: File): LinuxApp? {
        val lang = Locale.getDefault().language
        val full = lang + "_" + Locale.getDefault().country

        var inEntry = false
        val kv = HashMap<String, String>()
        val names = HashMap<String, String>()
        val comments = HashMap<String, String>()
        val text = runCatching { file.readText() }.getOrNull() ?: return null
        for (raw in text.lineSequence()) {
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#")) continue
            if (line.startsWith("[")) {
                if (inEntry) break
                inEntry = line == "[Desktop Entry]"
                continue
            }
            if (!inEntry) continue
            val i = line.indexOf('=')
            if (i <= 0) continue
            val k = line.substring(0, i).trim()
            val v = line.substring(i + 1).trim()
            when {
                k.startsWith("Name[") -> names[k.substring(5).removeSuffix("]")] = v
                k.startsWith("Comment[") -> comments[k.substring(8).removeSuffix("]")] = v
                !k.contains('[') -> kv[k] = v
            }
        }

        if (kv["Type"] != "Application") return null
        if (kv["NoDisplay"].equals("true", true) || kv["Hidden"].equals("true", true)) return null
        val only = kv["OnlyShowIn"]?.split(';')?.filter { it.isNotBlank() }.orEmpty()
        if (only.isNotEmpty() && only.none { it.equals("KDE", true) }) return null
        val not = kv["NotShowIn"]?.split(';').orEmpty()
        if (not.any { it.equals("KDE", true) }) return null

        val exec = kv["Exec"]?.takeIf { it.isNotBlank() } ?: return null
        kv["TryExec"]?.takeIf { it.isNotBlank() }?.let { if (!guestHasBinary(it)) return null }

        val name = (names[full] ?: names[lang] ?: kv["Name"]).orEmpty().ifBlank { file.nameWithoutExtension }
        val comment = (comments[full] ?: comments[lang] ?: kv["Comment"]).orEmpty()

        return LinuxApp(
            id = file.nameWithoutExtension,
            name = name,
            comment = comment,
            exec = exec,
            icon = kv["Icon"].orEmpty(),
            categories = kv["Categories"]?.split(';')?.filter { it.isNotBlank() }.orEmpty(),
            terminal = kv["Terminal"].equals("true", true),
        )
    }

    private fun guestHasBinary(name: String): Boolean {
        if (name.startsWith("/")) return File(rootfs, name.removePrefix("/")).exists()
        return listOf("usr/local/bin", "usr/bin", "bin", "usr/local/sbin", "usr/sbin", "sbin", "usr/games")
            .any { File(rootfs, "$it/$name").exists() }
    }


    private fun sizeOf(f: File): Int {
        val rel = "/" + f.relativeTo(rootfs).path
        Regex("/(\\d+)(?:x\\d+)?(?:@\\d+x?)?/").find(rel)?.groupValues?.get(1)?.toIntOrNull()?.let { return it }
        return if (rel.contains("/scalable/")) 0 else 32
    }

    private fun better(a: File?, b: File): File {
        if (a == null) return b
        fun score(f: File): Int {
            val s = sizeOf(f)
            return if (s in 1..256) s else if (s > 256) 1000 - s.coerceAtMost(999) else 0
        }
        return if (score(b) > score(a)) b else a
    }

    private fun resolveIcons(list: List<LinuxApp>) {
        val missing = list.mapNotNull { it.icon.takeIf { n -> n.isNotBlank() } }
            .filter { !iconFiles.containsKey(it) }
            .toMutableSet()
        if (missing.isEmpty()) return

        for (n in missing.toList()) {
            if (!n.startsWith("/")) continue
            val f = File(rootfs, n.removePrefix("/"))
            if (f.isFile && isBitmapName(f.name)) { iconFiles[n] = f; missing -= n }
        }

        val wanted = missing.associateBy { it.removeSuffix(".png").removeSuffix(".xpm") }
        val found = HashMap<String, File>()
        val roots = listOf(
            File(rootfs, "usr/share/icons"), File(rootfs, "usr/local/share/icons"),
            File(rootfs, "usr/share/pixmaps"), File(rootfs, "root/.local/share/icons"),
        ) + (File(rootfs, "home").listFiles()?.map { File(it, ".local/share/icons") } ?: emptyList())

        for (r in roots) {
            if (!r.isDirectory) continue
            r.walkTopDown().maxDepth(6).forEach { f ->
                if (!f.isFile || !f.name.endsWith(".png")) return@forEach
                val base = f.name.removeSuffix(".png")
                if (base !in wanted) return@forEach
                val rel = "/" + f.relativeTo(rootfs).path
                if (rel.contains("/apps/") || rel.contains("/pixmaps/")) {
                    found[base] = better(found[base], f)
                }
            }
        }
        for ((base, orig) in wanted) found[base]?.let { iconFiles[orig] = it }
    }

    private fun isBitmapName(n: String) = n.endsWith(".png") || n.endsWith(".jpg") || n.endsWith(".jpeg") || n.endsWith(".webp")

    fun icon(id: String): ImageBitmap {
        synchronized(iconBitmaps) { iconBitmaps[id]?.let { return it } }
        val a = _apps.value.firstOrNull { it.id == id }
        val file = a?.icon?.let { iconFiles[it] }
        val bmp = file?.let { f ->
            runCatching {
                val o = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeFile(f.path, o)
                val opts = BitmapFactory.Options().apply {
                    var s = 1
                    while (o.outWidth / (s * 2) >= 192) s *= 2
                    inSampleSize = s
                }
                BitmapFactory.decodeFile(f.path, opts)?.asImageBitmap()
            }.getOrNull()
        } ?: letterIcon(a?.name ?: id)
        synchronized(iconBitmaps) { iconBitmaps[id] = bmp }
        return bmp
    }

    private fun letterIcon(name: String): ImageBitmap {
        val size = 128
        val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        val hue = (name.fold(0) { acc, ch -> acc * 31 + ch.code } and 0x7fffffff) % 360
        val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = android.graphics.Color.HSVToColor(floatArrayOf(hue.toFloat(), .55f, .72f)) }
        c.drawRoundRect(RectF(4f, 4f, size - 4f, size - 4f), 26f, 26f, bg)
        val tx = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = android.graphics.Color.WHITE; textSize = 70f; textAlign = Paint.Align.CENTER; isFakeBoldText = true
        }
        c.drawText(name.firstOrNull { it.isLetterOrDigit() }?.uppercase() ?: "?", size / 2f, size / 2f + 25f, tx)
        return bmp.asImageBitmap()
    }


    fun commandLine(a: LinuxApp): String {
        val sb = StringBuilder()
        var i = 0
        val e = a.exec
        while (i < e.length) {
            val ch = e[i]
            if (ch == '%' && i + 1 < e.length) {
                if (e[i + 1] == '%') sb.append('%')
                i += 2
            } else { sb.append(ch); i++ }
        }
        var cmd = sb.toString().trim().replace(Regex("\\s+"), " ")
        val bin = cmd.substringBefore(' ').substringAfterLast('/')
        val needsNoSandbox = Regex("^(chromium(-browser)?|google-chrome.*|chrome|brave.*|vivaldi.*|microsoft-edge.*|opera|code|code-oss|codium|electron.*|discord|slack|obsidian)$")
            .matches(bin)
        if (needsNoSandbox && !cmd.contains("--no-sandbox")) {
            cmd = if (cmd.contains(' ')) cmd.replaceFirst(" ", " --no-sandbox ") else "$cmd --no-sandbox"
        }
        return cmd
    }

    fun launch(a: LinuxApp) {
        val d = app.debian
        val tmp = File(d.paths.rootfs, "tmp")
        runCatching { app.wayland.ensureStarted(d.paths) }
        val x11 = a.id != "pi-apps" && app.settings.string(Keys.GFX_SERVER, "wayland") == "x11"
        if (x11) app.x11.watch(tmp)
        val display = if (x11) runCatching { app.x11.ensure(tmp) }.getOrDefault(-1) else -1
        val env = app.wayland.clientEnvironment(x11, display.takeIf { it >= 0 })
        val binds = app.wayland.clientBinds(d.paths)

        val script = "export XDG_RUNTIME_DIR=\"\${XDG_RUNTIME_DIR:-/tmp/runtime-root}\"; " +
            "mkdir -p \"\$XDG_RUNTIME_DIR\" 2>/dev/null; chmod 0700 \"\$XDG_RUNTIME_DIR\" 2>/dev/null; " +
            "if [ -x /usr/local/bin/plasma-gl ]; then eval \"\$(/usr/local/bin/plasma-gl env --no-probe 2>/dev/null)\"; fi; " +
            "cd \"\$HOME\" 2>/dev/null; exec " + commandLine(a)

        ProotLauncher.prepare(d.paths)
        d.refreshDns()
        val cmd = ProotLauncher.command(
            d.paths, d.paths.rootfs, listOf("/bin/bash", "-c", script),
            binds = binds, guestEnv = env, workdir = "/root",
        )
        val log = File(tmp, "linux-app-${a.id}.log")
        val pb = ProcessBuilder(cmd).directory(d.paths.root).redirectErrorStream(true)
        pb.redirectOutput(ProcessBuilder.Redirect.to(log))
        pb.environment().apply { clear(); putAll(ProotLauncher.hostEnv(d.paths)) }
        val p = pb.start()
        runCatching { p.outputStream.close() }
    }
}
