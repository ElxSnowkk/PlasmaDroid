
package org.plasma.app.session

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.withStyle

class TermScheme(val id: String, val label: String, val bg: Color, val fg: Color, val ansi: List<Color>, val selection: Color, val cursor: Color)

object TermSchemes {
    private fun c(v: Long) = Color(0xFF000000 or v)
    val BREEZE = TermScheme("breeze", "Breeze", c(0x232627), c(0xFCFCFC),
        listOf(c(0x232627), c(0xED1515), c(0x11D116), c(0xF67400), c(0x1D99F3), c(0x9B59B6), c(0x1ABC9C), c(0xFCFCFC),
            c(0x7F8C8D), c(0xC0392B), c(0x1CDC9A), c(0xFDBC4B), c(0x3DAEE9), c(0x8E44AD), c(0x16A085), c(0xFFFFFF)),
        Color(0x663DAEE9), c(0xFCFCFC))
    val LIGHT = TermScheme("light", "Breeze Claro", c(0xFCFCFC), c(0x232627),
        listOf(c(0x232627), c(0xED1515), c(0x11D116), c(0xF67400), c(0x1D99F3), c(0x9B59B6), c(0x1ABC9C), c(0xC4C5C5),
            c(0x7F8C8D), c(0xC0392B), c(0x1CDC9A), c(0xFDBC4B), c(0x3DAEE9), c(0x8E44AD), c(0x16A085), c(0xFFFFFF)),
        Color(0x663DAEE9), c(0x232627))
    val SOLARIZED = TermScheme("solarized", "Solarized Dark", c(0x002B36), c(0x839496),
        listOf(c(0x073642), c(0xDC322F), c(0x859900), c(0xB58900), c(0x268BD2), c(0xD33682), c(0x2AA198), c(0xEEE8D5),
            c(0x002B36), c(0xCB4B16), c(0x586E75), c(0x657B83), c(0x839496), c(0x6C71C4), c(0x93A1A1), c(0xFDF6E3)),
        Color(0x66268BD2), c(0x93A1A1))
    val ALL = listOf(BREEZE, LIGHT, SOLARIZED)
    fun byId(id: String) = ALL.firstOrNull { it.id == id } ?: BREEZE
}

object Ansi {
    private val cache = object : LinkedHashMap<String, AnnotatedString>(512, 0.75f, true) {
        override fun removeEldestEntry(e: MutableMap.MutableEntry<String, AnnotatedString>?) = size > 1500
    }

    private val OSC_RE = Regex("\u001b\\][^\u0007\u001b]*(?:\u0007|\u001b\\\\)")
    private val CSI_RE = Regex("\u001b\\[[0-?]*[ -/]*[@-~]")
    private val ESC2_RE = Regex("\u001b[()*+#%]?[0-9A-Za-z=>]")

    fun strip(s: String): String = s.replace(OSC_RE, "").replace(CSI_RE, "").replace(ESC2_RE, "")

    @Synchronized
    fun render(raw: String, scheme: TermScheme): AnnotatedString {
        val key = scheme.id + "\u0000" + raw
        cache[key]?.let { return it }
        return parse(raw, scheme).also { cache[key] = it }
    }

    private fun xterm256(n: Int, s: TermScheme): Color = when {
        n < 16 -> s.ansi[n]
        n < 232 -> {
            val i = n - 16
            fun lv(v: Int) = if (v == 0) 0 else 55 + v * 40
            Color(lv(i / 36), lv((i / 6) % 6), lv(i % 6))
        }
        else -> { val g = 8 + (n - 232) * 10; Color(g, g, g) }
    }

    private class St {
        var fg: Color? = null; var bg: Color? = null
        var bold = false; var dim = false; var italic = false; var underline = false; var reverse = false
    }

    private fun parse(raw: String, s: TermScheme): AnnotatedString {
        val st = St()
        return buildAnnotatedString {
            var i = 0
            val text = StringBuilder()
            fun flush() {
                if (text.isEmpty()) return
                var fg = st.fg ?: s.fg
                var bg = st.bg
                val f0 = st.fg
                if (st.bold && f0 != null) {
                    val idx = s.ansi.indexOf(f0)
                    if (idx in 0..7) fg = s.ansi[idx + 8]
                }
                if (st.reverse) { val t = fg; fg = bg ?: s.bg; bg = t }
                if (st.dim) fg = fg.copy(alpha = 0.6f)
                withStyle(SpanStyle(
                    color = fg, background = bg ?: Color.Unspecified,
                    fontWeight = if (st.bold) FontWeight.Bold else null,
                    fontStyle = if (st.italic) FontStyle.Italic else null,
                    textDecoration = if (st.underline) TextDecoration.Underline else null,
                )) { append(text.toString()) }
                text.setLength(0)
            }
            while (i < raw.length) {
                val ch = raw[i]
                if (ch == '\u001b' && i + 1 < raw.length && raw[i + 1] == '[') {
                    var j = i + 2
                    while (j < raw.length && raw[j] in '0'..'?') j++
                    while (j < raw.length && raw[j] in ' '..'/') j++
                    if (j >= raw.length) break
                    val fin = raw[j]
                    if (fin == 'm') {
                        flush()
                        val p = raw.substring(i + 2, j).split(';', ':').map { it.filter(Char::isDigit).toIntOrNull() ?: 0 }.ifEmpty { listOf(0) }
                        var k = 0
                        while (k < p.size) {
                            when (val c = p[k]) {
                                0 -> { st.fg = null; st.bg = null; st.bold = false; st.dim = false; st.italic = false; st.underline = false; st.reverse = false }
                                1 -> st.bold = true
                                2 -> st.dim = true
                                3 -> st.italic = true
                                4 -> st.underline = true
                                7 -> st.reverse = true
                                22 -> { st.bold = false; st.dim = false }
                                23 -> st.italic = false
                                24 -> st.underline = false
                                27 -> st.reverse = false
                                in 30..37 -> st.fg = s.ansi[c - 30]
                                39 -> st.fg = null
                                in 40..47 -> st.bg = s.ansi[c - 40]
                                49 -> st.bg = null
                                in 90..97 -> st.fg = s.ansi[c - 90 + 8]
                                in 100..107 -> st.bg = s.ansi[c - 100 + 8]
                                38, 48 -> {
                                    val mode = p.getOrNull(k + 1)
                                    val col: Color? = when (mode) {
                                        5 -> p.getOrNull(k + 2)?.let { xterm256(it.coerceIn(0, 255), s) }.also { k += 2 }
                                        2 -> if (k + 4 < p.size) Color(p[k + 2].coerceIn(0, 255), p[k + 3].coerceIn(0, 255), p[k + 4].coerceIn(0, 255)).also { k += 4 } else null
                                        else -> null
                                    }
                                    if (col != null) { if (c == 38) st.fg = col else st.bg = col }
                                }
                            }
                            k++
                        }
                    }
                    i = j + 1
                } else if (ch == '\u001b' && i + 1 < raw.length && raw[i + 1] == ']') {

                    var e = i + 2; var next = raw.length
                    while (e < raw.length) {
                        if (raw[e] == '\u0007') { next = e + 1; break }
                        if (raw[e] == '\u001b' && e + 1 < raw.length && raw[e + 1] == '\\') { next = e + 2; break }
                        e++
                    }
                    i = next
                } else if (ch == '\u001b') {
                    i += if (i + 1 < raw.length && raw[i + 1] in "()*+#%") 3 else 2
                } else if (ch == '\r' || ch == '\u0007') i++
                else if (ch == '\t') { text.append("        ".substring(0, 8 - (length + text.length) % 8)); i++ }
                else { text.append(ch); i++ }
            }
            flush()
        }
    }
}
