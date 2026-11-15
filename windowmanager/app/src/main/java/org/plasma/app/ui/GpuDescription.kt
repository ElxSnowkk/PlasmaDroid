// Parte do projeto PlasmaDroid.

package org.plasma.app.ui

import org.plasma.app.session.DesktopSession
import org.plasma.render.RenderBackend

private val GENERIC = setOf("", "desconhecida", "dispositivo vulkan", "unknown", "n/a")

private fun clean(raw: String?): String =
    raw.orEmpty().substringBefore(" · ").filter { !it.isISOControl() }.trim()

fun gpuName(session: DesktopSession): String {
    val probes = session.app.render.selection.value?.probes
    val candidates = sequenceOf(
        session.compositor.info.value.renderer,
        probes?.get(RenderBackend.VULKAN)?.details?.get("vk.device"),
        probes?.get(RenderBackend.GLES)?.details?.get("gles.renderer"),
    )
    return candidates.map(::clean).firstOrNull { it.lowercase() !in GENERIC } ?: "desconhecida"
}

fun gpuDescription(session: DesktopSession): String = gpuName(session)
