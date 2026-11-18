
package org.plasma.bridge

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import org.plasma.core.PlasmaLog
import java.util.concurrent.TimeUnit

class RootBridge {
    private val _available = MutableStateFlow<Boolean?>(null)
    val available: StateFlow<Boolean?> = _available.asStateFlow()
    val backend: PrivilegedBackend = RootBackend { cmd, t -> su(cmd, t) }

    suspend fun detect(): Boolean = withContext(Dispatchers.IO) {
        val out = su(listOf("id"), 8000)
        val ok = out.contains("uid=0")
        PlasmaLog.i("root", "deteccao: ${if (ok) "root disponivel" else "sem root"} (${out.take(80).replace('\n', ' ')})")
        _available.value = ok
        ok
    }

    private fun su(cmd: List<String>, timeoutMs: Int): String = try {
        val p = ProcessBuilder("su", "-c", cmd.joinToString(" ") { "'" + it.replace("'", "'\\''") + "'" })
            .redirectErrorStream(true).start()
        if (!p.waitFor(timeoutMs.toLong(), TimeUnit.MILLISECONDS)) { p.destroy(); "timeout" }
        else p.inputStream.readBytes().decodeToString()
    } catch (t: Throwable) { "erro: $t" }
}
