
package org.plasma.app.ui

import android.content.res.Configuration
import androidx.compose.foundation.*
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.plasma.app.session.DesktopSession
import org.plasma.app.session.linuxKickoffCategory
import org.plasma.app.session.MenuItem
import org.plasma.app.session.MenuState
import org.plasma.core.Keys

private val TOOLS = listOf("tool:files", "tool:terminal", "tool:settings", "tool:capture")

private val KICKOFF_CATEGORIES = listOf(
    "Favoritos" to "star",
    "Todos os aplicativos" to "list",
    "Linux" to "terminal",
    "Desenvolvimento" to "terminal",
    "Jogos" to "chip",
    "Gráficos" to "palette",
    "Ajuda" to "info",
    "Internet" to "share",
    "Multimídia" to "panel",
    "Escritório" to "file",
    "Sistema" to "chip",
    "Utilitários" to "folder",
)

@Composable
fun Kickoff(session: DesktopSession, modifier: Modifier) {
    val st = session.app.settings
    st.watch()
    val b = LocalBreeze.current
    val apps by session.app.apps.collectAsState()
    val linuxApps by session.app.linuxApps.apps.collectAsState()
    var query by remember { mutableStateOf("") }
    var category by remember { mutableStateOf("Favoritos") }
    val favs = st.list(Keys.FAVS, "tool:files,tool:terminal,tool:settings")
    val all = remember(apps, linuxApps) { TOOLS + apps.map { "app:${it.pkg}" } + linuxApps.map { "linux:${it.id}" } }
    LaunchedEffect(Unit) { session.app.linuxApps.requestRefresh(0) }
    val results = remember(query, apps, linuxApps) {
        if (query.isBlank()) emptyList()
        else all.filter { entryLabel(it, apps).contains(query.trim(), ignoreCase = true) }
    }
    val selected = remember(category, apps, linuxApps, favs, all) {
        when (category) {
            "Favoritos" -> favs
            "Todos os aplicativos" -> all
            "Linux" -> all.filter { it.startsWith("linux:") }
            else -> all.filter { id ->
                if (id.startsWith("tool:")) kickoffToolCategory(id) == category
                else if (id.startsWith("linux:")) linuxApps.firstOrNull { "linux:${it.id}" == id }?.let { linuxKickoffCategory(it) == category } == true
                else apps.firstOrNull { it.pkg == id.removePrefix("app:") }?.let { kickoffAppCategory(it.pkg, it.label) == category } == true
            }
        }
    }
    val focus = remember { FocusRequester() }
    val hasKeyboard = LocalConfiguration.current.keyboard != Configuration.KEYBOARD_NOKEYS
    LaunchedEffect(Unit) { if (hasKeyboard) runCatching { focus.requestFocus() } }
    val user = st.string(Keys.USER_NAME, "Usuário")

    val fx = LocalFx.current
    val open = remember { Animatable(0f) }
    LaunchedEffect(Unit) { session.glass.kick(fx.ms(Motion.LONG) + 80); open.animateTo(1f, Motion.spec(fx, Motion.LONG + 40)) }
    PopupSurface(
        modifier.width(560.dp).height(480.dp).graphicsLayer {
            val v = open.value
            alpha = v
            val sc = .96f + .04f * v
            scaleX = sc
            scaleY = sc
            translationY = (1f - v) * 24f
            transformOrigin = androidx.compose.ui.graphics.TransformOrigin(0f, 1f)
        },
        z = 25f,
        alpha = { open.value },
    ) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(34.dp).clip(CircleShape).background(b.accent), contentAlignment = Alignment.Center) {
                T(user.take(1).uppercase(), 15.sp, Color.White, FontWeight.Bold)
            }
            Column(Modifier.padding(start = 10.dp).weight(1f)) {
                T(user, 13.sp, weight = FontWeight.SemiBold, maxLines = 1)
                T("KDE Plasma", 10.sp, b.dim, maxLines = 1)
            }
            Box(
                Modifier.width(230.dp).clip(RoundedCornerShape(5.dp))
                    .background(b.view.mul(.85f)).border(1.dp, b.line, RoundedCornerShape(5.dp))
                    .padding(horizontal = 9.dp, vertical = 8.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    BreezeIcon("search", 15.dp, b.dim)
                    Spacer(Modifier.width(7.dp))
                    Box(Modifier.weight(1f)) {
                        if (query.isEmpty()) T("Pesquisar aplicativos…", 12.sp, b.dim)
                        BasicTextField(
                            query,
                            { query = it },
                            Modifier.fillMaxWidth().focusRequester(focus),
                            singleLine = true,
                            textStyle = TextStyle(color = b.text, fontSize = 12.sp),
                            cursorBrush = SolidColor(b.accent),
                            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                            keyboardActions = KeyboardActions(onSearch = { results.firstOrNull()?.let { openById(session, it) } }),
                        )
                    }
                }
            }
        }

        BoxWithConstraints(Modifier.weight(1f).fillMaxWidth().background(b.view.mul(.48f))) {

            val compact = maxWidth < 520.dp
            val entries = if (query.isNotBlank()) results else selected
            val grid: @Composable (Modifier) -> Unit = { m ->
                Box(m.padding(5.dp)) {
                    if (entries.isEmpty()) {
                        T(
                            if (query.isNotBlank()) "Nenhum aplicativo encontrado."
                            else "Nenhum aplicativo nesta categoria.",
                            12.sp, b.dim, modifier = Modifier.align(Alignment.Center),
                        )
                    } else {
                        EntryGrid(session, entries, apps, favs)
                    }
                }
            }
            if (compact) {
                Column(Modifier.fillMaxSize()) {
                    Row(
                        Modifier.fillMaxWidth().background(b.window.mul(.48f)).horizontalScroll(rememberScrollState())
                            .padding(horizontal = 5.dp, vertical = 5.dp),
                    ) {
                        KICKOFF_CATEGORIES.forEach { (label, icon) ->
                            val active = query.isBlank() && category == label
                            Row(
                                Modifier.padding(horizontal = 2.dp).clip(RoundedCornerShape(16.dp))
                                    .background(if (active) b.accent.copy(alpha = .22f) else Color.Transparent)
                                    .clickable { category = label; query = "" }
                                    .padding(horizontal = 11.dp, vertical = 8.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                BreezeIcon(icon, 15.dp, if (active) b.accent else b.dim)
                                T(label, 11.sp, if (active) b.text else b.dim,
                                    if (active) FontWeight.SemiBold else null, Modifier.padding(start = 7.dp), maxLines = 1)
                            }
                        }
                    }
                    grid(Modifier.weight(1f).fillMaxWidth())
                }
            } else {
                Row(Modifier.fillMaxSize()) {
                    Column(
                        Modifier.width(166.dp).fillMaxHeight().background(b.window.mul(.48f))
                            .verticalScroll(rememberScrollState()).padding(vertical = 6.dp, horizontal = 5.dp),
                    ) {
                        KICKOFF_CATEGORIES.forEach { (label, icon) ->
                            val active = query.isBlank() && category == label
                            Row(
                                Modifier.fillMaxWidth().padding(vertical = 1.dp).clip(RoundedCornerShape(4.dp))
                                    .background(if (active) b.accent.copy(alpha = .22f) else Color.Transparent)
                                    .clickable { category = label; query = "" }
                                    .padding(horizontal = 8.dp, vertical = 9.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                BreezeIcon(icon, 15.dp, if (active) b.accent else b.dim)
                                T(label, 11.sp, if (active) b.text else b.dim,
                                    if (active) FontWeight.SemiBold else null,
                                    Modifier.padding(start = 9.dp), maxLines = 1)
                            }
                        }
                    }
                    grid(Modifier.weight(1f).fillMaxHeight())
                }
            }
        }

        Row(
            Modifier.fillMaxWidth().background(b.window.mul(.72f)).padding(horizontal = 6.dp, vertical = 3.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FooterAction("Configurações") { session.openTool("settings") }
            Spacer(Modifier.weight(1f))
            FooterAction("Bloquear") { session.closePopups(); session.locked = true }
            FooterAction("Sair") { session.closePopups(); session.leaveDesktop() }
        }
    }
}

private fun kickoffToolCategory(id: String): String = when (id) {
    "tool:terminal" -> "Desenvolvimento"
    "tool:capture" -> "Gráficos"
    "tool:settings" -> "Sistema"
    else -> "Utilitários"
}

private fun kickoffAppCategory(pkg: String, label: String): String {
    val key = "$pkg $label".lowercase()
    return when {
        listOf("termux", "ide", "developer", "code editor", "adb", "vscode").any { key.contains(it) } -> "Desenvolvimento"
        listOf("game", "games", "play games", "minecraft", "roblox").any { key.contains(it) } -> "Jogos"
        listOf("camera", "gallery", "photos", "video", "music", "youtube", "spotify").any { key.contains(it) } -> "Multimídia"
        listOf("chrome", "firefox", "browser", "internet", "webview").any { key.contains(it) } -> "Internet"
        listOf("docs", "sheets", "office", "word", "excel", "pdf").any { key.contains(it) } -> "Escritório"
        listOf("settings", "systemui", "launcher", "packageinstaller").any { key.contains(it) } -> "Sistema"
        listOf("paint", "photo editor", "image editor", "drawing").any { key.contains(it) } -> "Gráficos"
        listOf("help", "support", "faq", "manual", "guide").any { key.contains(it) } -> "Ajuda"
        else -> "Utilitários"
    }
}

@Composable
private fun FooterAction(label: String, onClick: () -> Unit) {
    Box(Modifier.padding(vertical = 4.dp).breezeClickable(5.dp) { onClick() }.padding(horizontal = 10.dp, vertical = 8.dp)) { T(label, 12.sp) }
}

@Composable
private fun EntryGrid(session: DesktopSession, ids: List<String>, apps: List<org.plasma.bridge.AndroidApp>, favs: List<String>) {
    LazyVerticalGrid(GridCells.Fixed(3), Modifier.fillMaxSize(), contentPadding = PaddingValues(4.dp)) {
        items(ids, key = { it }) { id ->
            Column(
                Modifier.padding(2.dp).breezeClickable(6.dp) { openById(session, id) }
                    .onSecondaryClick { p -> entryMenu(session, id, favs, p) }
                    .pointerInput(id) { detectTapGestures(onLongPress = { p -> entryMenu(session, id, favs, p) }) { openById(session, id) } }
                    .padding(8.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                EntryIcon(id, U.iconL)
                T(entryLabel(id, apps), 11.sp, maxLines = 2, align = TextAlign.Center, modifier = Modifier.padding(top = 4.dp))
            }
        }
    }
}

@Composable
private fun AllApps(session: DesktopSession, apps: List<org.plasma.bridge.AndroidApp>, favs: List<String>) {
    val b = LocalBreeze.current
    val grouped = remember(apps) { apps.groupBy { it.label.firstOrNull()?.uppercaseChar()?.takeIf { c -> c.isLetter() } ?: '#' }.toSortedMap() }
    LazyColumn(Modifier.fillMaxSize()) {
        item { Row(Modifier.fillMaxWidth().breezeClickable(4.dp) { openById(session, "tool:files") }.padding(10.dp)) { ToolIcon("files", 28.dp); T("Dolphin", modifier = Modifier.padding(start = 12.dp)) } }
        item { Row(Modifier.fillMaxWidth().breezeClickable(4.dp) { openById(session, "tool:terminal") }.padding(10.dp)) { ToolIcon("terminal", 28.dp); T("Konsole", modifier = Modifier.padding(start = 12.dp)) } }
        item { Row(Modifier.fillMaxWidth().breezeClickable(4.dp) { openById(session, "tool:settings") }.padding(10.dp)) { ToolIcon("settings", 28.dp); T("Configurações do Sistema", modifier = Modifier.padding(start = 12.dp)) } }
        grouped.forEach { (letter, list) ->
            item(key = "h$letter") { T("$letter", 12.sp, b.accent, FontWeight.Bold, Modifier.padding(start = 14.dp, top = 10.dp, bottom = 2.dp)) }
            items(list, key = { it.pkg }) { a ->
                Row(Modifier.fillMaxWidth().breezeClickable(4.dp) { session.openAndroid(a) }.onSecondaryClick { p -> entryMenu(session, "app:${a.pkg}", favs, p) }
                    .pointerInput(a.pkg) { detectTapGestures(onLongPress = { p -> entryMenu(session, "app:${a.pkg}", favs, p) }) { session.openAndroid(a) } }
                    .padding(horizontal = 10.dp, vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                    AppIcon(a, 28.dp); T(a.label, modifier = Modifier.padding(start = 12.dp), maxLines = 1)
                }
            }
        }
    }
}

private fun entryMenu(session: DesktopSession, id: String, favs: List<String>, p: androidx.compose.ui.geometry.Offset) {
    val st = session.app.settings
    val pinned = st.list(Keys.PINNED, "tool:files,tool:terminal,tool:settings")
    val desk = st.list(Keys.DESK_ICONS, "tool:files,tool:terminal,tool:settings")
    session.menu = MenuState(p.x, p.y, buildList {
        add(MenuItem("Abrir") { openById(session, id) })
        if (id.startsWith("app:")) add(MenuItem("Abrir fora do desktop (tela cheia)") {
            session.app.apps.value.firstOrNull { "app:${it.pkg}" == id }?.let { a ->
                runCatching { session.app.startActivity(session.app.catalog.launchIntent(a)) }
                session.closePopups()
            }
        })
        add(MenuItem("", separator = true))
        add(if (id in favs) MenuItem("Remover dos favoritos") { st.putList(Keys.FAVS, favs - id) } else MenuItem("Adicionar aos favoritos") { st.putList(Keys.FAVS, favs + id) })
        add(if (id in pinned) MenuItem("Desafixar da barra de tarefas") { st.putList(Keys.PINNED, pinned - id) } else MenuItem("Fixar na barra de tarefas") { st.putList(Keys.PINNED, pinned + id) })
        if (id !in desk) add(MenuItem("Adicionar à área de trabalho") { st.putList(Keys.DESK_ICONS, desk + id) })
    })
}
