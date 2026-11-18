
package org.plasma.app.ui

import androidx.compose.foundation.*
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.zIndex
import org.plasma.app.R
import org.plasma.app.session.DesktopSession

@Composable
fun AppImageDialog(session: DesktopSession) {
    val p = session.appImagePrompt ?: return
    val b = LocalBreeze.current
    val shape = RoundedCornerShape(8.dp)
    Box(
        Modifier.fillMaxSize().zIndex(2500f).background(Color.Black.copy(alpha = .38f))
            .pointerInput(Unit) { detectTapGestures { session.appImagePrompt = null } },
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.widthIn(max = 440.dp).fillMaxWidth(.92f).clip(shape).background(b.window).border(1.dp, b.line, shape)
                .pointerInput(Unit) { detectTapGestures { } }.padding(18.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                val icon = p.icon
                if (icon != null) Image(icon, null, Modifier.size(72.dp), contentScale = ContentScale.Fit)
                else Image(painterResource(R.drawable.appimage_fallback), null, Modifier.size(72.dp), contentScale = ContentScale.Fit)
                Column(Modifier.padding(start = 14.dp)) {
                    T("Executar AppImage?", 17.sp, weight = FontWeight.Bold)
                    T(p.title.ifBlank { p.file.name }, 14.sp, b.accent, maxLines = 2)
                    if (p.comment.isNotBlank()) T(p.comment, 12.sp, b.dim, maxLines = 2)
                }
            }
            Spacer(Modifier.height(12.dp))
            T("Este arquivo é uma AppImage: um aplicativo Linux portátil que roda sem instalação, dentro do Debian (PRoot). " +
                "Ele será aberto no Konsole. Execute apenas AppImages de fontes em que você confia.", 13.sp)
            Spacer(Modifier.height(10.dp))
            T("Arquivo: ${p.file.name}", 12.sp, b.dim, maxLines = 2)
            val arch = when {
                p.loading -> "Arquitetura: verificando… (procurando o ícone)"
                p.arch.isBlank() -> "Arquitetura: não identificada"
                p.archOk -> "Arquitetura: ${p.arch} (compatível)"
                else -> "Arquitetura: ${p.arch}; este aparelho é ${p.hostArch}. Provavelmente não vai executar."
            }
            T(arch, 12.sp, if (!p.loading && !p.archOk) b.negative else b.dim)
            Spacer(Modifier.height(16.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End)) {
                BreezeButton("Cancelar", { session.appImagePrompt = null })
                BreezeButton("Executar", { session.runAppImage(p) }, primary = true)
            }
        }
    }
}
