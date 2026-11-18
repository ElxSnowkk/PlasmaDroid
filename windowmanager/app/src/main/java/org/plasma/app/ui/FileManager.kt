
package org.plasma.app.ui

import android.content.Intent
import android.os.Build
import android.os.Environment
import android.provider.Settings
import android.webkit.MimeTypeMap
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.FileProvider
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.MenuItem
import org.plasma.app.session.MenuState
import org.plasma.core.Keys
import kotlinx.coroutines.ensureActive
import java.io.File
import java.text.DateFormat
import java.util.Date

@Composable
fun FileManager(session: DesktopSession) {
    val ctx = LocalContext.current
    val b = LocalBreeze.current
    val root = remember { Environment.getExternalStorageDirectory() }
    var dir by remember { mutableStateOf(root) }
    var showHidden by remember { mutableStateOf(false) }
    var rename by remember { mutableStateOf<File?>(null) }
    var newFolder by remember { mutableStateOf(false) }
    var tick by remember { mutableIntStateOf(0) }
    val back = remember { ArrayDeque<File>() }
    val hasAccess = remember(dir, tick) { Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager() }
    val entries by produceState(emptyList<FileRow>(), dir, showHidden, tick) {
        kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
            val df = DateFormat.getDateInstance(DateFormat.SHORT)
            val files = (dir.listFiles()?.toList() ?: emptyList()).filter { showHidden || !it.name.startsWith(".") }

            val rows = files.map { f ->
                val d = f.isDirectory
                FileRow(f, d, if (d) "…" else humanSize(f.length()), df.format(Date(f.lastModified())),
                    !d && f.extension.lowercase() in IMG_EXT)
            }.sortedWith(compareBy<FileRow>({ !it.dir }, { it.file.name.lowercase() }))
            value = rows

            if (rows.any { it.dir }) {
                val out = rows.toMutableList()
                var n = 0
                for (i in rows.indices) {
                    val r = rows[i]
                    if (!r.dir) continue
                    ensureActive()
                    out[i] = FileRow(r.file, true, "${r.file.list()?.size ?: 0} itens", r.date, false)
                    if (++n % 24 == 0) value = out.toList()
                }
                value = out.toList()
            }
        }
    }
    fun go(f: File) { if (f != dir) { back.addLast(dir); dir = f } }

    Row(Modifier.fillMaxSize()) {
        Column(Modifier.width(150.dp).fillMaxHeight().background(b.window).padding(6.dp)) {
            T("Lugares", 11.sp, b.dim, modifier = Modifier.padding(6.dp))
            listOf("Início" to root, "Downloads" to File(root, "Download"), "Imagens" to File(root, "Pictures"), "Câmera" to File(root, "DCIM"),
                "Músicas" to File(root, "Music"), "Vídeos" to File(root, "Movies"), "Documentos" to File(root, "Documents")).forEach { (n, f) ->
                Box(Modifier.fillMaxWidth().background(if (dir == f) b.accent.copy(alpha = .3f) else androidx.compose.ui.graphics.Color.Transparent)
                    .clickable { go(f) }.padding(8.dp)) { T(n, 13.sp) }
            }
        }
        Column(Modifier.weight(1f).fillMaxHeight()) {
            Row(Modifier.fillMaxWidth().background(b.window).padding(4.dp), verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(30.dp).breezeClickable(5.dp, enabled = back.isNotEmpty()) { back.removeLastOrNull()?.let { dir = it } }, contentAlignment = Alignment.Center) { BreezeIcon("arrowL", 16.dp, if (back.isNotEmpty()) b.text else b.dim.copy(alpha = .4f)) }
                Spacer(Modifier.width(4.dp))
                Box(Modifier.size(30.dp).breezeClickable(5.dp, enabled = dir != root) { dir.parentFile?.takeIf { dir != root }?.let { go(it) } }, contentAlignment = Alignment.Center) { BreezeIcon("arrowUp", 16.dp, if (dir != root) b.text else b.dim.copy(alpha = .4f)) }
                Spacer(Modifier.width(8.dp))
                Row(Modifier.weight(1f).horizontalScroll(rememberScrollState())) {
                    val parts = generateSequence(dir) { it.parentFile?.takeIf { p -> p.absolutePath.startsWith(root.absolutePath) && it != root } }.toList().reversed()
                    parts.forEach { f ->
                        Box(Modifier.clickable { go(f) }.padding(horizontal = 6.dp, vertical = 4.dp)) { T(if (f == root) "Início" else f.name, 13.sp, if (f == dir) b.text else b.accent) }
                        BreezeIcon("chevR", 12.dp, b.dim, Modifier.padding(top = 6.dp))
                    }
                }
                BreezeButton("Nova pasta", { newFolder = true })
                Spacer(Modifier.width(4.dp))
                BreezeButton(if (showHidden) "Ocultar ocultos" else "Mostrar ocultos", { showHidden = !showHidden })
            }
            if (!hasAccess) Row(Modifier.fillMaxWidth().background(b.accent.copy(alpha = .2f)).padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
                T("Sem acesso a todos os arquivos.", 12.sp, modifier = Modifier.weight(1f))
                BreezeButton("Conceder", {
                    ctx.startActivity(Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, android.net.Uri.parse("package:${ctx.packageName}")).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                })
            }
            if (newFolder || rename != null) {
                var name by remember(newFolder, rename) { mutableStateOf(rename?.name ?: "Nova pasta") }
                Row(Modifier.fillMaxWidth().padding(6.dp), verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.weight(1f).background(b.view).border(1.dp, b.line).padding(8.dp)) {
                        BasicTextField(name, { name = it }, Modifier.fillMaxWidth(), singleLine = true,
                            textStyle = TextStyle(color = b.text, fontSize = 13.sp), cursorBrush = SolidColor(b.accent))
                    }
                    Spacer(Modifier.width(6.dp))
                    BreezeButton("OK", {
                        val ok = if (newFolder) File(dir, name).mkdirs() else rename!!.renameTo(File(rename!!.parentFile, name))
                        if (!ok) session.showToast("Operação falhou (permissão ou nome já existe)")
                        newFolder = false; rename = null; tick++
                    }, primary = true)
                    Spacer(Modifier.width(4.dp))
                    BreezeButton("Cancelar", { newFolder = false; rename = null })
                }
            }
            LazyColumn(Modifier.fillMaxSize()) {
                items(entries, key = { it.file.absolutePath }, contentType = { if (it.dir) 0 else 1 }) { row ->
                    val f = row.file
                    Row(Modifier.fillMaxWidth()
                        .clickable { if (f.isDirectory) go(f) else openFile(session, f) }
                        .onSecondaryClick { p ->
                            session.menu = MenuState(p.x, p.y, buildList {
                                add(MenuItem("Abrir") { if (f.isDirectory) go(f) else openFile(session, f) })
                                if (row.image)
                                    add(MenuItem("Usar como papel de parede") { session.app.settings.put(Keys.WALLPAPER, android.net.Uri.fromFile(f).toString()) })
                                add(MenuItem("Renomear…") { rename = f })
                                add(MenuItem("", separator = true))
                                add(MenuItem("Excluir") {
                                    val ok = if (f.isDirectory) f.deleteRecursively() else f.delete()
                                    if (!ok) session.showToast("Não foi possível excluir"); tick++
                                })
                            })
                        }.padding(horizontal = 10.dp, vertical = 6.dp),
                        verticalAlignment = Alignment.CenterVertically) {
                        BreezeIcon(if (row.dir) "folder" else "file", 20.dp, if (row.dir) b.accent else b.dim)
                        T(f.name, 13.sp, modifier = Modifier.weight(1f).padding(start = 10.dp), maxLines = 1)
                        T(row.info, 11.sp, b.dim, modifier = Modifier.width(80.dp), maxLines = 1)
                        T(row.date, 11.sp, b.dim, modifier = Modifier.width(80.dp), maxLines = 1)
                    }
                }
            }
        }
    }
}

private class FileRow(val file: File, val dir: Boolean, val info: String, val date: String, val image: Boolean)
private val IMG_EXT = setOf("jpg", "jpeg", "png", "webp")

private fun humanSize(n: Long): String = when {
    n < 1024 -> "$n B"; n < 1024 * 1024 -> "%.1f KB".format(n / 1024f); n < 1024L * 1024 * 1024 -> "%.1f MB".format(n / 1048576f)
    else -> "%.2f GB".format(n / 1073741824f)
}

private fun openFile(session: DesktopSession, f: File) {
    val ctx = session.app
    try {
        val uri = FileProvider.getUriForFile(ctx, "${ctx.packageName}.files", f)
        val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(f.extension.lowercase()) ?: "*/*"
        ctx.startActivity(Intent(Intent.ACTION_VIEW).setDataAndType(uri, mime).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK))
    } catch (t: Throwable) { session.showToast("Nenhum aplicativo abre este arquivo") }
}
