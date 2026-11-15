// Parte do projeto PlasmaDroid.

package org.plasma.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch
import org.plasma.app.session.DesktopSession
import org.plasma.core.Keys
import org.plasma.proot.DebianEnvironment
import org.plasma.proot.PlasmaVKSupport
import org.plasma.proot.StepState

const val GRAPHICS_ENV_TITLE = "Graphics Environment (Vulkan And PlasmaGL)"

@Composable
internal fun GraphicsEnvironmentSection(session: DesktopSession, modifier: Modifier = Modifier) {
    val app = session.app
    val b = LocalBreeze.current
    val ui by app.debian.ui.collectAsState()
    val settingsVer by app.settings.version.collectAsState()
    val rootfs = app.debian.paths.rootfs

    val kr = remember(ui.complete, ui.steps, ui.running, settingsVer) { PlasmaVKSupport.describe(rootfs) }
    val gl = remember(ui.complete, ui.steps, ui.running, settingsVer) { PlasmaVKSupport.describePlasmaGl(rootfs) }
    val krStep = ui.steps.firstOrNull { it.id == DebianEnvironment.S_KR }
    val installed = remember(ui.steps, ui.complete) { app.debian.installed }
    var details by remember { mutableStateOf(false) }

    Column(modifier.fillMaxWidth()) {
        SectionTitle(GRAPHICS_ENV_TITLE)
        Column(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(9.dp)).background(b.view.copy(alpha = .68f))
                .border(1.dp, b.line, RoundedCornerShape(9.dp)).padding(12.dp),
        ) {
            if (krStep != null) {
                val (mark, col) = when (krStep.state) {
                    StepState.DONE -> "✔" to b.positive
                    StepState.RUNNING -> "●" to b.accent
                    StepState.FAILED -> "✘" to b.negative
                    StepState.PENDING -> "○" to b.dim
                }
                Row(verticalAlignment = Alignment.Top) {
                    T(mark, 14.sp, col, modifier = Modifier.width(22.dp))
                    Column(Modifier.weight(1f)) {
                        T("PlasmaVK · Vulkan", 13.sp, weight = FontWeight.Medium)
                        if (krStep.detail.isNotBlank()) T(krStep.detail, 11.sp, b.dim)
                    }
                }
            }
            kr.firstOrNull { it.first == "Estado" }?.let { (_, v) -> T(v, 11.sp, b.dim, modifier = Modifier.padding(top = 4.dp, start = 22.dp), maxLines = 3) }
            gl.forEach { (k, v) ->
                Row(Modifier.fillMaxWidth().padding(top = 4.dp)) {
                    T(k, 12.sp, b.dim, modifier = Modifier.width(128.dp))
                    T(v, 12.sp, maxLines = 3, modifier = Modifier.weight(1f))
                }
            }
            Row(
                Modifier.fillMaxWidth().padding(top = 6.dp).clip(RoundedCornerShape(7.dp)).breezeClickable(7.dp) { details = !details }.padding(vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                T("Detalhes do PlasmaVK", 12.sp, weight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                BreezeIcon(if (details) "arrowUp" else "chevR", 14.dp, b.dim)
            }
            if (details) kr.forEach { (k, v) ->
                Row(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
                    T(k, 12.sp, b.dim, modifier = Modifier.width(128.dp))
                    T(v, 12.sp, maxLines = 3, modifier = Modifier.weight(1f))
                }
            }
        }
        AngleSwitch(session, installed, settingsVer)
        WsiSwitch(session, busy = ui.running, installed = installed, settingsVer = settingsVer)
        VulkanDriverPicker(session, busy = ui.running, installed = installed, modifier = Modifier.padding(top = 8.dp))
    }
}

@Composable
internal fun VulkanDriverPicker(session: DesktopSession, busy: Boolean, installed: Boolean, modifier: Modifier = Modifier) {
    val app = session.app
    val st = app.settings
    val b = LocalBreeze.current
    val scope = rememberCoroutineScope()
    val ver = st.version.collectAsState().value
    val injected = remember(ver) { st.string(Keys.VK_INJECTED_BIONIC, "") }

    Column(
        modifier.fillMaxWidth().clip(RoundedCornerShape(9.dp)).background(b.view.copy(alpha = .68f))
            .border(1.dp, b.line, RoundedCornerShape(9.dp)).padding(12.dp),
    ) {
        T("Driver Vulkan", 13.sp, weight = FontWeight.SemiBold)
        T(
            if (injected.isNotBlank()) "Importado: $injected"
            else "Nenhum driver importado. Usa o Vulkan nativo do aparelho pelo libhybris.",
            11.sp, b.dim, modifier = Modifier.padding(top = 4.dp),
        )
        T(
            "Só drivers Bionic (Android) são aceitos: .so HAL (exporta HMI, ex.: vulkan.adreno.so), .so ICD ou um .zip com eles. Drivers glibc (Mesa PanVK/Turnip) são recusados.",
            11.sp, b.dim, modifier = Modifier.padding(top = 4.dp),
        )
        if (!installed) T("O Debian ainda não foi instalado. Termine o passo Ambiente antes de importar.", 11.sp, b.neutral, modifier = Modifier.padding(top = 4.dp))
        Row(Modifier.padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            BreezeButton("Importar Driver Vulkan", { session.pickVulkanDriver() }, enabled = !busy && installed, primary = true)
            if (injected.isNotBlank()) BreezeButton("Remover", {
                PlasmaVKSupport.clearInjectedBionic(app.debian.paths.rootfs)
                st.put(Keys.VK_INJECTED_BIONIC, "")
                st.put(Keys.VK_DRIVER, "auto")
                PlasmaVKSupport.driverPreference = "auto"
                session.vulkanImportStatus = ""
                scope.launch { app.debian.recheckVulkan() }
            }, enabled = !busy)
            BreezeButton("Reverificar", { scope.launch { app.debian.recheckVulkan() } }, enabled = !busy && installed)
        }
        if (session.vulkanImportStatus.isNotBlank())
            T(session.vulkanImportStatus, 11.sp, if (session.vulkanImportOk) b.accent else b.negative, modifier = Modifier.padding(top = 6.dp))
        T("Vulkan do aparelho: ${remember { PlasmaVKSupport.androidVulkanSummary() }}", 11.sp, b.dim, modifier = Modifier.padding(top = 6.dp))
        if (busy) T("Testando o driver…", 11.sp, b.accent, modifier = Modifier.padding(top = 4.dp))
        else T("Terminais e apps Linux já abertos continuam com o driver anterior; abra-os de novo para usar o novo.", 11.sp, b.dim, modifier = Modifier.padding(top = 4.dp))
    }
}

@Composable
private fun AngleSwitch(session: DesktopSession, installed: Boolean, settingsVer: Int) {
    val app = session.app
    val st = app.settings
    val b = LocalBreeze.current
    val rootfs = app.debian.paths.rootfs
    val on = remember(settingsVer) { st.bool(Keys.GL_ANGLE, false) }

    LaunchedEffect(installed, on) { if (installed && PlasmaVKSupport.angleEnabled(rootfs) != on) PlasmaVKSupport.setAngle(rootfs, on) }
    Column(Modifier.fillMaxWidth().padding(top = 8.dp).clip(RoundedCornerShape(9.dp)).background(b.view.copy(alpha = .68f))
        .border(1.dp, b.line, RoundedCornerShape(9.dp)).padding(horizontal = 12.dp, vertical = 6.dp)) {
        SwitchRow(
            "Usar ANGLE no OpenGL", on,
            "O PlasmaGL funciona sem ANGLE, direto no GLES do aparelho. Ligue para traduzir GLES → Vulkan pelo ANGLE. Vale para apps abertos depois.",
        ) { v ->
            st.put(Keys.GL_ANGLE, v)
            if (installed) PlasmaVKSupport.setAngle(rootfs, v)
        }
        if (on && installed && !PlasmaVKSupport.angleLibsPresent(rootfs))
            T("ANGLE ligado, mas libEGL_angle.so e libGLESv2_angle.so não estão no Debian; o Incandescent cai no GLES nativo.", 11.sp, b.neutral, modifier = Modifier.padding(bottom = 6.dp))
    }
}

@Composable
private fun WsiSwitch(session: DesktopSession, busy: Boolean, installed: Boolean, settingsVer: Int) {
    val app = session.app
    val st = app.settings
    val b = LocalBreeze.current
    val scope = rememberCoroutineScope()
    val on = remember(settingsVer) { st.bool(Keys.VK_WSI_DMABUF, false) }
    Column(Modifier.fillMaxWidth().padding(top = 8.dp).clip(RoundedCornerShape(9.dp)).background(b.view.copy(alpha = .68f))
        .border(1.dp, b.line, RoundedCornerShape(9.dp)).padding(horizontal = 12.dp, vertical = 6.dp)) {
        SwitchRow(
            "Apresentar Vulkan por dma-buf", on,
            "Desligado: a GPU renderiza em hardware e o quadro final sobe por wl_shm (estável). Ligado: o WSI usa dma-buf, sem cópia; se o driver não exportar dma-buf, os apps Vulkan não abrem janela. Vale para apps abertos depois.",
        ) { v ->
            st.put(Keys.VK_WSI_DMABUF, v)
            PlasmaVKSupport.wsiDmabuf = v
            if (installed && !busy) scope.launch { app.debian.recheckVulkan() }
        }
    }
}
