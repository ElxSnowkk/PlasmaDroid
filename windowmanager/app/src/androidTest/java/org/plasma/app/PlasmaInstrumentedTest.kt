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

package org.plasma.app

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.plasma.render.MSG_PROBE_GLES
import org.plasma.render.MSG_PROBE_VK
import org.plasma.render.ProbeClient
import org.plasma.render.RenderBackend
import org.plasma.sharing.CodecCaps

@RunWith(AndroidJUnit4::class)
class PlasmaInstrumentedTest {
    private val ctx = InstrumentationRegistry.getInstrumentation().targetContext
    private val app get() = PlasmaApp.of(ctx)

    @Test fun vulkanProbeCompletesWithoutHanging() = runBlocking {
        val r = ProbeClient(ctx).run(MSG_PROBE_VK)
        assertTrue("sonda Vulkan deve terminar (sucesso ou falha explicada)", r.isSuccess || r.exceptionOrNull()?.message != null)
    }

    @Test fun glesProbeWorksOnAnyAndroidDevice() = runBlocking {
        val r = ProbeClient(ctx).run(MSG_PROBE_GLES)
        assertTrue(r.isSuccess)
        assertEquals("1", r.getOrThrow()["gles.available"])
        assertEquals("ok", r.getOrThrow()["gles.selftest"])
    }

    @Test fun renderSelectionAlwaysYieldsAUsableBackend() = runBlocking {
        val s = app.render.evaluate()
        assertTrue(s.active in listOf(RenderBackend.VULKAN, RenderBackend.GLES, RenderBackend.SOFTWARE))
    }

    @Test fun forcedSoftwareFallsBackToSoftware() = runBlocking {
        app.settings.put(org.plasma.core.Keys.RENDER, "software")
        assertEquals(RenderBackend.SOFTWARE, app.render.evaluate().active)
        app.settings.put(org.plasma.core.Keys.RENDER, "auto")
    }

    @Test fun defaultDisplayIsDetected() {
        app.displays.refresh()
        assertTrue(app.displays.displays.value.any { it.isDefault })
    }

    @Test fun windowCreationAndFocus() {
        val s = app.sessions.obtain(9999)
        s.openTool("files")
        val st = s.wm.state.value
        assertEquals(1, st.windows.size); assertEquals(st.windows[0].id, st.focusedId)
        s.shutdown(); app.sessions.release(9999)
    }

    @Test fun h264EncoderExists() { assertTrue(CodecCaps.isAvailable(org.plasma.sharing.VideoCodec.H264)) }
}
