/*
 * This file belongs to the PlasmaDroid project.
 *
 * Copyright (C) 2026 ElxSnowkk (https://github.com/ElxSnowkk)
 *
 * PlasmaDroid is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * Licensed under GPL-3.0-or-later. See the LICENSE file at the project root,
 * or <https://www.gnu.org/licenses/gpl-3.0.html>.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 */

package org.plasma.render

import android.content.Context
import android.content.pm.PackageManager
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.plasma.core.Keys
import org.plasma.core.PlasmaLog
import org.plasma.core.SettingsStore

enum class RenderBackend(val id: String, val label: String) {
    AUTO("auto", "Automático"), VULKAN("vulkan", "Vulkan"), GLES("gles", "OpenGL ES"), SOFTWARE("software", "Software");
    companion object { fun of(id: String) = entries.firstOrNull { it.id == id } ?: AUTO }
}

data class BackendProbe(val backend: RenderBackend, val usable: Boolean, val reason: String, val details: Map<String, String>)
data class RenderSelection(
    val requested: RenderBackend,
    val active: RenderBackend,
    val probes: Map<RenderBackend, BackendProbe>,
    val log: List<String>,
    val systemInfo: Map<String, String>,
)

class RenderBackendManager(private val ctx: Context, private val settings: SettingsStore) {
    private val client = ProbeClient(ctx)
    private val _selection = MutableStateFlow<RenderSelection?>(null)
    val selection: StateFlow<RenderSelection?> = _selection.asStateFlow()

    suspend fun evaluate(): RenderSelection {
        val requested = RenderBackend.of(settings.string(Keys.RENDER, "auto"))
        val log = ArrayList<String>()
        val probes = LinkedHashMap<RenderBackend, BackendProbe>()
        val chain = when (requested) {
            RenderBackend.AUTO, RenderBackend.VULKAN -> listOf(RenderBackend.VULKAN, RenderBackend.GLES, RenderBackend.SOFTWARE)
            RenderBackend.GLES -> listOf(RenderBackend.GLES, RenderBackend.SOFTWARE)
            RenderBackend.SOFTWARE -> listOf(RenderBackend.SOFTWARE)
        }
        for (b in chain) {
            val p = probe(b)
            probes[b] = p
            log += "${b.label}: ${if (p.usable) "OK" else "indisponível"} — ${p.reason}"
            if (!p.usable && requested != RenderBackend.AUTO && b == requested) {
                log += "Backend pedido falhou; aplicando fallback automático"
            }
        }
        val active = chain.firstOrNull { probes[it]?.usable == true } ?: RenderBackend.SOFTWARE
        val pm = ctx.packageManager
        val sys = linkedMapOf(
            "Vulkan (feature do sistema)" to (pm.systemAvailableFeatures.firstOrNull { it.name == PackageManager.FEATURE_VULKAN_HARDWARE_VERSION }
                ?.let { "versão 0x%x".format(it.version) } ?: "não declarado"),
            "OpenGL ES (reqGlEsVersion)" to "0x%x".format(
                (ctx.getSystemService(Context.ACTIVITY_SERVICE) as android.app.ActivityManager).deviceConfigurationInfo.reqGlEsVersion),
        )
        val sel = RenderSelection(requested, active, probes, log, sys)
        settings.put("render.active", active.id)
        PlasmaLog.i("render", "pedido=${requested.id} ativo=${active.id} :: ${log.joinToString(" | ")}")
        _selection.value = sel
        return sel
    }

    fun runtimeFallback(reason: String) {
        val current = _selection.value ?: return
        if (current.active != RenderBackend.VULKAN) return
        val gles = current.probes[RenderBackend.GLES]
        val fallback = if (gles?.usable == true) RenderBackend.GLES else RenderBackend.SOFTWARE
        val message = "Vulkan falhou em runtime ($reason); fallback para ${fallback.label}"
        val next = current.copy(active = fallback, log = current.log + message)
        settings.put("render.active", fallback.id)
        _selection.value = next
        PlasmaLog.w("render", message)
    }

    private suspend fun probe(b: RenderBackend): BackendProbe = when (b) {
        RenderBackend.SOFTWARE -> BackendProbe(b, true, "renderização por software (HWUI/Skia CPU) — último recurso", emptyMap())
        RenderBackend.VULKAN -> {
            val r = client.run(MSG_PROBE_VK)
            r.fold({ m ->
                val major = m["vk.apiMajor"]?.toIntOrNull() ?: 0
                val minor = m["vk.apiMinor"]?.toIntOrNull() ?: 0
                val compositorBuilt = runCatching { NativeProbe.hasVulkanCompositor() }.getOrDefault(false)
                when {
                    m["vk.available"] != "1" -> BackendProbe(b, false, m["vk.error"] ?: "sem Vulkan", m)
                    m["vk.selftest"] != "ok" -> BackendProbe(b, false, "teste de execução falhou: ${m["vk.selftest"]}", m)
                    major == 1 && minor < 1 -> BackendProbe(b, false, "somente Vulkan 1.0 (exigido 1.1+)", m)
                    m["vk.ext.swapchain"] != "1" -> BackendProbe(b, false, "sem VK_KHR_swapchain", m)
                    !compositorBuilt -> BackendProbe(b, false, "shader-tools ausente no build: compositor SPIR-V não compilado", m)
                    else -> BackendProbe(b, true, "${m["vk.device"]} · Vulkan $major.$minor · swapchain + SPIR-V", m + ("vk.compositor" to "1"))
                }
            }, { BackendProbe(b, false, it.message ?: "falha na sonda", emptyMap()) })
        }
        RenderBackend.GLES -> {
            val r = client.run(MSG_PROBE_GLES)
            r.fold({ m ->
                when {
                    m["gles.available"] != "1" -> BackendProbe(b, false, m["gles.error"] ?: "sem GLES", m)
                    m["gles.selftest"] != "ok" -> BackendProbe(b, false, "teste de execução falhou: ${m["gles.selftest"]}", m)
                    else -> BackendProbe(b, true, "${m["gles.renderer"]} · ES ${m["gles.contextVersion"]}", m)
                }
            }, { BackendProbe(b, false, it.message ?: "falha na sonda", emptyMap()) })
        }
        RenderBackend.AUTO -> BackendProbe(b, false, "n/a", emptyMap())
    }
}
