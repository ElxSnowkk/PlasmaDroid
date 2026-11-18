
package org.plasma.app.ui

import android.graphics.BitmapFactory
import android.net.Uri
import android.util.LruCache
import androidx.compose.foundation.*
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.Animatable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsHoveredAsState
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.IntOffset
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.*
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import org.plasma.app.session.DesktopSession
import org.plasma.bridge.AndroidApp
import org.plasma.core.SettingsStore
import kotlin.math.cos
import kotlin.math.sin

val LocalSession = staticCompositionLocalOf<DesktopSession> { error("sem sessão") }

@Composable fun SettingsStore.watch(): Int = version.collectAsState().value

@Composable
fun T(
    text: String, size: TextUnit = 13.sp, color: Color = LocalBreeze.current.text, weight: FontWeight? = null,
    modifier: Modifier = Modifier, maxLines: Int = Int.MAX_VALUE, align: TextAlign? = null,
) = Text(text, modifier, color = color, fontSize = size, fontWeight = weight, maxLines = maxLines,
    overflow = TextOverflow.Ellipsis, textAlign = align)

fun Modifier.onSecondaryClick(block: (Offset) -> Unit): Modifier = composed {
    var origin by remember { mutableStateOf(Offset.Zero) }
    this.onGloballyPositioned { origin = it.positionInRoot() }.pointerInput(Unit) {
        awaitPointerEventScope {
            while (true) {
                val e = awaitPointerEvent(PointerEventPass.Initial)
                if (e.type == PointerEventType.Press && e.buttons.isSecondaryPressed) {
                    block(origin + e.changes.first().position)
                    e.changes.forEach { it.consume() }
                }
            }
        }
    }
}

@Composable
fun rememberNow(periodMs: Long): State<Long> = produceState(System.currentTimeMillis(), periodMs) {
    while (true) { value = System.currentTimeMillis(); delay(periodMs - System.currentTimeMillis() % periodMs) }
}

object IconCache { val cache = LruCache<String, ImageBitmap>(300) }

@Composable
fun AppIcon(a: AndroidApp, size: Dp) {
    val app = LocalSession.current.app
    val bmp by produceState(IconCache.cache.get(a.pkg), a.pkg) {
        if (value == null) {
            val b = withContext(Dispatchers.IO) { app.catalog.icon(a, 128)?.asImageBitmap() }
            if (b != null) { IconCache.cache.put(a.pkg, b); value = b }
        }
    }
    bmp?.let { Image(it, null, Modifier.size(size)) } ?: Spacer(Modifier.size(size))
}

@Composable
fun ToolIcon(kind: String, size: Dp) {
    val b = LocalBreeze.current
    Canvas(Modifier.size(size)) {
        val w = this.size.width
        when (kind) {
            "files" -> {
                drawRoundRect(Brush.verticalGradient(listOf(Color(0xFF2B9FE8), Color(0xFF1A7FC4))), Offset(w * .08f, w * .13f), Size(w * .42f, w * .26f), CornerRadius(w * .06f))
                drawRoundRect(Brush.verticalGradient(listOf(Color(0xFF6CC8F5), Color(0xFF2FA4EC), Color(0xFF1D8FDB))), Offset(w * .06f, w * .26f), Size(w * .88f, w * .60f), CornerRadius(w * .09f))
                drawRoundRect(Color(0x55FFFFFF), Offset(w * .10f, w * .29f), Size(w * .80f, w * .035f), CornerRadius(w * .02f))
            }
            "settings" -> {

                val c = Offset(w / 2, w / 2)
                for (i in 0 until 8) {
                    val a = i * Math.PI / 4
                    drawLine(Color(0xFF8E9AA0), Offset(c.x + (w * .22f * cos(a)).toFloat(), c.y + (w * .22f * sin(a)).toFloat()),
                        Offset(c.x + (w * .34f * cos(a)).toFloat(), c.y + (w * .34f * sin(a)).toFloat()), strokeWidth = w * .12f, cap = StrokeCap.Round)
                }
                drawCircle(Brush.verticalGradient(listOf(Color(0xFFB6C0C5), Color(0xFF7F8C8D))), w * .27f, c)
                drawCircle(Color(0xFF31363B), w * .105f, c)
                drawCircle(Color(0xFF3DAEE9), w * .055f, c)
            }
            "terminal" -> {
                drawRoundRect(Brush.verticalGradient(listOf(Color(0xFF4D565E), Color(0xFF232629))), Offset(w * .05f, w * .12f), Size(w * .90f, w * .76f), CornerRadius(w * .10f))
                drawRoundRect(Color(0xFF1B1E20), Offset(w * .10f, w * .27f), Size(w * .80f, w * .56f), CornerRadius(w * .05f))
                drawRect(Color(0x66FFFFFF), Offset(w * .10f, w * .19f), Size(w * .80f, w * .02f))
                val sw = w * .07f
                drawLine(Color(0xFF3DAEE9), Offset(w * .20f, w * .42f), Offset(w * .34f, w * .53f), sw, StrokeCap.Round)
                drawLine(Color(0xFF3DAEE9), Offset(w * .34f, w * .53f), Offset(w * .20f, w * .64f), sw, StrokeCap.Round)
                drawLine(Color(0xFFFCFCFC), Offset(w * .44f, w * .66f), Offset(w * .68f, w * .66f), sw, StrokeCap.Round)
            }
            "capture" -> {
                drawRoundRect(Brush.verticalGradient(listOf(Color(0xFF5BC0F0), Color(0xFF1D99F3))), Offset(w * .06f, w * .18f), Size(w * .88f, w * .62f), CornerRadius(w * .08f))
                drawCircle(Color(0xFF1B6FB5), w * .19f, Offset(w * .5f, w * .49f))
                drawCircle(Color(0xFFFCFCFC), w * .13f, Offset(w * .5f, w * .49f))
                drawRoundRect(Color(0xFF1D99F3), Offset(w * .30f, w * .10f), Size(w * .22f, w * .10f), CornerRadius(w * .03f))
            }
            else -> drawCircle(b.accent, w * .4f)
        }
    }
}

@Composable
fun EntryIcon(id: String, size: Dp) {
    if (id.startsWith("tool:")) ToolIcon(id.removePrefix("tool:"), size)
    else if (id.startsWith("wayland:") || id.startsWith("x11:")) {
        val bmp = LocalSession.current.guestIcons[id]
        if (bmp != null) Image(bmp, null, Modifier.size(size)) else Spacer(Modifier.size(size))
    } else if (id.startsWith("linux:")) {
        val catalog = LocalSession.current.app.linuxApps
        val list by catalog.apps.collectAsState()
        val bmp by produceState<ImageBitmap?>(null, id, list) {
            value = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) { catalog.icon(id.removePrefix("linux:")) }
        }
        val b = bmp
        if (b != null) Image(b, null, Modifier.size(size)) else Spacer(Modifier.size(size))
    } else {
        val apps by LocalSession.current.app.apps.collectAsState()
        val a = apps.firstOrNull { it.pkg == id.removePrefix("app:") }
        if (a != null) AppIcon(a, size) else Spacer(Modifier.size(size))
    }
}

fun entryLabel(id: String, apps: List<AndroidApp>): String = when (id) {
    "tool:files" -> "Dolphin"
    "tool:settings" -> "Configurações do Sistema"
    "tool:terminal" -> "Konsole"
    "tool:capture" -> "Captura de tela"
    else -> if (id.startsWith("linux:")) org.plasma.app.session.LinuxAppIndex.byId[id]?.name ?: id.removePrefix("linux:")
    else apps.firstOrNull { it.pkg == id.removePrefix("app:") }?.label ?: id.removePrefix("app:")
}

fun openById(session: DesktopSession, id: String) {
    if (id.startsWith("tool:")) session.openTool(id.removePrefix("tool:"))
    else if (id.startsWith("linux:")) org.plasma.app.session.LinuxAppIndex.byId[id]?.let { session.openLinux(it) }
        ?: session.showToast("Aplicativo não encontrado")
    else session.app.apps.value.firstOrNull { it.pkg == id.removePrefix("app:") }?.let { session.openAndroid(it) }
        ?: session.showToast("Aplicativo não encontrado")
}

val LocalWinAlpha = compositionLocalOf<() -> Float> { { 1f } }

@Composable
fun Modifier.breezeClickable(
    radius: Dp = 4.dp, accent: Boolean = false, enabled: Boolean = true, onClick: () -> Unit,
): Modifier {
    val b = LocalBreeze.current
    val fx = LocalFx.current
    val src = remember { MutableInteractionSource() }
    val hover by src.collectIsHoveredAsState()
    val pressed by src.collectIsPressedAsState()
    val target = when { !enabled -> 0f; pressed -> 1f; hover -> 0.7f; else -> 0f }
    val a by animateFloatAsState(target, Motion.spec(fx, Motion.SHORT), label = "hover")
    val base = if (accent) b.accent else b.text
    val shape = RoundedCornerShape(radius)
    return this.clip(shape)
        .drawBehind { drawRect(base.copy(alpha = (if (accent) 1f else 0.14f) * a)) }
        .hoverable(src).clickable(interactionSource = src, indication = null, enabled = enabled) { onClick() }
}

@Composable
fun BreezeButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier, primary: Boolean = false, enabled: Boolean = true) {
    val b = LocalBreeze.current
    val fx = LocalFx.current
    val src = remember { MutableInteractionSource() }
    val hover by src.collectIsHoveredAsState()
    val pressed by src.collectIsPressedAsState()
    val h by animateFloatAsState(if (pressed) 1f else if (hover) .6f else 0f, Motion.spec(fx, Motion.SHORT), label = "btn")
    val shape = RoundedCornerShape(5.dp)
    val top = if (primary) b.accent else b.button
    Box(
        modifier.graphicsLayer { alpha = if (enabled) 1f else .4f }.clip(shape)
            .background(Brush.verticalGradient(listOf(top.copy(alpha = 1f), top.copy(alpha = 1f)))).drawBehind { drawRect(b.text.copy(alpha = .07f * h)) }
            .border(1.dp, if (hover || primary) b.accent.copy(alpha = if (primary) 1f else .55f + .45f * h) else b.line, shape)
            .hoverable(src).clickable(interactionSource = src, indication = null, enabled = enabled) { onClick() }
            .padding(horizontal = 14.dp, vertical = 6.dp),
        contentAlignment = Alignment.Center,
    ) { Text(text, color = if (primary) b.onAccent else b.text, fontSize = 13.sp, maxLines = 1, softWrap = false, overflow = TextOverflow.Ellipsis) }
}

@Composable
fun SectionTitle(text: String) {
    T(text, 15.sp, LocalBreeze.current.accent, FontWeight.SemiBold, Modifier.padding(top = 14.dp, bottom = 4.dp))
}

@Composable
fun BreezeSwitch(checked: Boolean, onChange: (Boolean) -> Unit, modifier: Modifier = Modifier) {
    val b = LocalBreeze.current
    val fx = LocalFx.current
    val p by animateFloatAsState(if (checked) 1f else 0f, Motion.spec(fx, Motion.LONG), label = "sw")
    val track = androidx.compose.ui.graphics.lerp(b.button, b.accent, p)
    Box(modifier.size(38.dp, 20.dp).clip(RoundedCornerShape(10.dp)).background(track)
        .border(1.dp, androidx.compose.ui.graphics.lerp(b.line, b.accent, p), RoundedCornerShape(10.dp))
        .clickable(indication = null, interactionSource = remember { MutableInteractionSource() }) { onChange(!checked) }) {
        Box(Modifier.offset(x = (3 + 18 * p).dp, y = 3.dp).size(14.dp).clip(CircleShape)
            .background(androidx.compose.ui.graphics.lerp(b.dim, Color(0xFFFCFCFC), p)))
    }
}

@Composable
fun BreezeSlider(value: Float, onChange: (Float) -> Unit, range: ClosedFloatingPointRange<Float> = 0f..1f, modifier: Modifier = Modifier, steps: Int = 0) {
    val b = LocalBreeze.current
    var w by remember { mutableIntStateOf(1) }
    val frac = ((value - range.start) / (range.endInclusive - range.start)).coerceIn(0f, 1f)
    fun commit(x: Float) {
        var f = (x / w).coerceIn(0f, 1f)
        if (steps > 0) f = (f * (steps + 1)).let { Math.round(it) } / (steps + 1f)
        onChange(range.start + f * (range.endInclusive - range.start))
    }
    Box(modifier.height(24.dp).onSizeChanged { w = it.width.coerceAtLeast(1) }
        .pointerInput(range, steps) { detectTapGestures { commit(it.x) } }
        .pointerInput(range, steps) { detectHorizontalDragGestures { c, _ -> c.consume(); commit(c.position.x) } },
        contentAlignment = Alignment.CenterStart) {
        Box(Modifier.fillMaxWidth().height(4.dp).clip(RoundedCornerShape(2.dp)).background(b.line.copy(alpha = .6f)))
        Box(Modifier.fillMaxWidth(frac).height(4.dp).clip(RoundedCornerShape(2.dp)).background(b.accent))
        val d = LocalDensity.current
        Box(Modifier.offset { IntOffset(((w * frac) - with(d) { 9.dp.toPx() }).toInt(), 0) }.size(18.dp).clip(CircleShape)
            .background(b.window).border(2.dp, b.accent, CircleShape))
    }
}

@Composable
fun PopupSurface(modifier: Modifier = Modifier, z: Float = 20f, kind: Float = 1f, alpha: () -> Float = { 1f }, content: @Composable ColumnScope.() -> Unit) {
    val b = LocalBreeze.current
    val shape = RoundedCornerShape(8.dp)
    Column(modifier.glass(b.window, 8.dp, shadow = 22.dp, shadowAlpha = .5f, kind = kind, z = z, alpha = alpha)
        .border(1.dp, b.line.copy(alpha = .75f), shape).pointerInput(Unit) { detectTapGestures { } }, content = content)
}

@Composable
fun ContextMenuHost(session: DesktopSession) {
    val m = session.menu ?: return
    val b = LocalBreeze.current
    val fx = LocalFx.current
    val d = LocalDensity.current
    val p = remember(m) { Animatable(0f) }
    LaunchedEffect(m) { session.glass.kick(fx.ms(Motion.LONG) + 60); p.animateTo(1f, Motion.spec(fx, Motion.SHORT + 40)) }
    BoxWithConstraints(Modifier.fillMaxSize().pointerInput(Unit) { detectTapGestures { session.menu = null } }) {
        val wPx = with(d) { 236.dp.toPx() }
        val hInt: Int = m.items.fold(12) { acc, item -> acc + (if (item.separator) 8 else 30) }
        val hPx = with(d) { hInt.dp.toPx() }
        val x = m.x.coerceIn(0f, (constraints.maxWidth - wPx).coerceAtLeast(0f))
        val y = m.y.coerceIn(0f, (constraints.maxHeight - hPx).coerceAtLeast(0f))
        PopupSurface(
            Modifier.offset { IntOffset(x.toInt(), y.toInt()) }.width(236.dp)
                .graphicsLayer { alpha = p.value; val s = .96f + .04f * p.value; scaleX = s; scaleY = s; transformOrigin = androidx.compose.ui.graphics.TransformOrigin(0f, 0f) }
                .padding(vertical = 5.dp),
            z = 40f, kind = 1.25f, alpha = { p.value },
        ) {
            m.items.forEach { it ->
                if (it.separator) Box(Modifier.fillMaxWidth().padding(vertical = 3.dp, horizontal = 10.dp).height(1.dp).background(b.line.copy(alpha = .7f)))
                else {
                    val src = remember { MutableInteractionSource() }
                    val hov by src.collectIsHoveredAsState()
                    val pr by src.collectIsPressedAsState()
                    val on = (hov || pr) && it.enabled
                    Box(Modifier.fillMaxWidth().padding(horizontal = 4.dp).height(30.dp).clip(RoundedCornerShape(4.dp))
                        .background(if (on) b.accent else Color.Transparent)
                        .hoverable(src).clickable(interactionSource = src, indication = null, enabled = it.enabled) { session.menu = null; it.action() }
                        .padding(horizontal = 10.dp), contentAlignment = Alignment.CenterStart) {
                        val fg = if (!it.enabled) b.dim else if (on) b.onAccent else b.text
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            if (m.items.any { x -> x.icon != null }) { if (it.icon != null) BreezeIcon(it.icon, 16.dp, fg) else Spacer(Modifier.size(16.dp)); Spacer(Modifier.width(8.dp)) }
                            T(it.label, 13.sp, fg, maxLines = 1)
                        }
                    }
                }
            }
        }
    }
}

fun loadBitmap(ctx: android.content.Context, uri: String, maxSide: Int): ImageBitmap? = try {
    val u = Uri.parse(uri)
    val o = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    ctx.contentResolver.openInputStream(u)?.use { BitmapFactory.decodeStream(it, null, o) }
    var s = 1
    while (o.outWidth / s > maxSide || o.outHeight / s > maxSide) s *= 2
    ctx.contentResolver.openInputStream(u)?.use { BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = s }) }?.asImageBitmap()
} catch (_: Throwable) { null }
