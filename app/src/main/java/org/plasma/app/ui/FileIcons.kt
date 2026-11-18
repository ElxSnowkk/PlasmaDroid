
package org.plasma.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.plasma.app.R
import org.plasma.app.session.AppImageSupport
import java.io.File

private class FKind(val color: Color, val stroke: String = "", val fill: String = "")

private fun k(rgb: Long, stroke: String = "", fill: String = "") = FKind(Color(0xFF000000 or rgb), stroke, fill)

private const val G_TEXT = "M5.5 8.4 H11.5 M5.5 10.6 H11.5 M5.5 12.8 H9"
private const val G_C = "M10.8 8.6 A3 3 0 1 0 10.8 12.4"
private const val G_CPP = "M9 8.9 A2.5 2.5 0 1 0 9 12.1 M10.2 10.5 H12.4 M11.3 9.4 V11.6"
private const val G_PY = "M5.6 12.8 V8.2 H7.4 A1.5 1.5 0 0 1 7.4 11.2 H5.6 M9.3 9 L10.6 11.7 L11.9 9 M10.6 11.7 L9.9 13.2"
private const val G_JAR = "M5.5 8.5 H10.5 V11 A2 2 0 0 1 8.5 13 H7.5 A2 2 0 0 1 5.5 11 Z M10.5 9.2 H11.5 A1 1 0 0 1 11.5 11.2 H10.5"
private const val G_ZIP = "M7.4 6.6 H8.6 M7.4 8.1 H8.6 M7.4 9.6 H8.6 M6.8 11 H9.2 V13.3 H6.8 Z"
private const val G_IMG = "M5 13 L7.8 9.5 L9.6 11.6 L10.8 10.4 L12 13 Z M6.6 8.3 h.01"
private const val G_AUDIO = "M6.5 12.8 A1.3 1.3 0 1 0 6.5 10.2 A1.3 1.3 0 0 0 6.5 12.8 M7.8 11.5 V7.9 L11.5 7.1"
private const val G_PDF = "M5.6 12.8 V8.3 H7.6 A1.6 1.6 0 0 1 7.6 11.5 H5.6 M10 8.3 V12.8 M10 8.3 H12"
private const val G_CODE = "M7 8.3 L5 10.6 L7 12.9 M10 8.3 L12 10.6 L10 12.9"
private const val G_SH = "M5.5 8.6 L8 10.6 L5.5 12.6 M9 12.9 H12"
private const val G_JSON = "M7.5 8 Q6.3 8 6.3 9.2 V10 Q6.3 10.5 5.5 10.6 Q6.3 10.7 6.3 11.2 V11.8 Q6.3 13 7.5 13 M9.5 8 Q10.7 8 10.7 9.2 V10 Q10.7 10.5 11.5 10.6 Q10.7 10.7 10.7 11.2 V11.8 Q10.7 13 9.5 13"
private const val G_APK = "M5 13 A3 3 0 0 1 11 13 Z M5.8 8.7 L6.6 9.9 M10.2 8.7 L9.4 9.9"
private const val G_DEB = "M5.5 8.5 L8 7.3 L10.5 8.5 V11.8 L8 13 L5.5 11.8 Z M5.5 8.5 L8 9.7 L10.5 8.5 M8 9.7 V13"
private const val G_ISO = "M8 13 A2.7 2.7 0 1 0 8 7.6 A2.7 2.7 0 0 0 8 13 Z M8 10.3 h.01"
private const val G_XLS = "M5.5 8 H11.5 V13 H5.5 Z M5.5 10.5 H11.5 M8.5 8 V13"
private const val G_PPT = "M5.5 8 H11.5 V11.5 H5.5 Z M8.5 11.5 V13.3 M6.5 13.3 H10.5"

private val GENERIC = k(0x8A949E)

private fun kindOf(ext: String): FKind = when (ext) {
    "txt", "log", "text", "nfo", "rtf" -> k(0x78909C, G_TEXT)
    "md", "markdown", "rst" -> k(0x546E7A, G_TEXT)
    "c", "h" -> k(0x4B72B8, G_C)
    "cpp", "cc", "cxx", "hpp", "hh", "hxx" -> k(0x1E5FA8, G_CPP)
    "py", "pyw", "pyc" -> k(0x3776AB, G_PY)
    "jar", "java", "class", "war" -> k(0xE76F00, G_JAR)
    "zip", "tar", "gz", "tgz", "xz", "txz", "bz2", "tbz2", "zst", "7z", "rar", "lz", "lzma", "z", "cab" -> k(0xD9A400, G_ZIP)
    "jpg", "jpeg", "png", "gif", "webp", "bmp", "svg", "ico", "heic", "tif", "tiff" -> k(0x2E9E6B, G_IMG)
    "mp3", "ogg", "flac", "wav", "m4a", "opus", "aac", "wma" -> k(0x8E44AD, G_AUDIO)
    "mp4", "mkv", "webm", "avi", "mov", "flv", "wmv", "3gp" -> k(0xD14B4B, fill = "M6.8 8 L11.5 10.5 L6.8 13 Z")
    "pdf" -> k(0xD32F2F, G_PDF)
    "html", "htm", "css" -> k(0xE34F26, G_CODE)
    "js", "mjs", "ts", "jsx", "tsx" -> k(0xC9A800, G_CODE)
    "kt", "kts", "go", "rs", "rb", "php", "lua", "cs", "swift", "sql", "pl", "dart" -> k(0x00897B, G_CODE)
    "sh", "bash", "zsh", "fish", "bat", "cmd" -> k(0x3C4A57, G_SH)
    "json", "yaml", "yml", "toml", "ini", "conf", "cfg", "xml", "properties" -> k(0x7B8794, G_JSON)
    "apk", "aab", "xapk" -> k(0x3DDC84, G_APK)
    "deb", "rpm", "pkg" -> k(0xD70A53, G_DEB)
    "iso", "img", "dmg" -> k(0x607D8B, G_ISO)
    "doc", "docx", "odt" -> k(0x2B579A, G_TEXT)
    "xls", "xlsx", "ods", "csv" -> k(0x217346, G_XLS)
    "ppt", "pptx", "odp" -> k(0xD24726, G_PPT)
    else -> GENERIC
}

private const val PAGE = "M4 1.5 H9.5 L13.5 5.5 V14 A.5 .5 0 0 1 13 14.5 H4 A.5 .5 0 0 1 3.5 14 V2 A.5 .5 0 0 1 4 1.5 Z"
private const val FOLD = "M9.5 1.5 V5.5 H13.5 Z"
private val fpCache = HashMap<String, Path>()
private fun fp(d: String): Path = fpCache.getOrPut(d) { PathParser().parsePathString(d).toPath() }

@Composable
fun FileTypeIcon(f: File, size: Dp = 22.dp) {
    val ext = f.extension.lowercase()

    val appImage = remember(f.absolutePath, f.lastModified()) {
        ext == "appimage" || (ext.isEmpty() && f.length() > 100_000 && AppImageSupport.isAppImage(f))
    }
    if (appImage) {
        Image(painterResource(R.drawable.appimage_fallback), null, Modifier.size(size), contentScale = ContentScale.Fit)
        return
    }
    val kind = remember(ext) { kindOf(ext) }
    Canvas(Modifier.size(size)) {
        val s = this.size.width / 16f
        withTransform({ scale(s, s, Offset.Zero) }) {
            drawPath(fp(PAGE), kind.color)
            drawPath(fp(FOLD), Color(0x55FFFFFF))
            if (kind.stroke.isNotEmpty()) drawPath(fp(kind.stroke), Color.White, style = Stroke(1.3f, cap = StrokeCap.Round, join = StrokeJoin.Round))
            if (kind.fill.isNotEmpty()) drawPath(fp(kind.fill), Color.White)
        }
    }
}
