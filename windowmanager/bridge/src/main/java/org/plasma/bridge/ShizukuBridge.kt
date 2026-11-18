
package org.plasma.bridge

import android.content.ComponentName
import android.content.Context
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.IBinder
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.plasma.core.PlasmaLog
import rikka.shizuku.Shizuku

enum class ShizukuState(val label: String) {
    NOT_INSTALLED("Shizuku não instalado"),
    NOT_RUNNING("Shizuku instalado, mas não está em execução"),
    NO_PERMISSION("Permissão do Shizuku pendente"),
    CONNECTING("Conectando ao serviço privilegiado…"),
    CONNECTED("Conectado (uid shell)"),
    ERROR("Erro ao conectar"),
}

class ShizukuBridge(private val ctx: Context) {
    private val _state = MutableStateFlow(ShizukuState.NOT_INSTALLED)
    val state: StateFlow<ShizukuState> = _state.asStateFlow()
    @Volatile private var service: IPrivilegedService? = null
    @Volatile private var backend: ShizukuBackend? = null

    private val onBinder = Shizuku.OnBinderReceivedListener { refresh() }
    private val onDead = Shizuku.OnBinderDeadListener { service = null; backend = null; refresh() }
    private val onPerm = Shizuku.OnRequestPermissionResultListener { _, _ -> refresh() }
    private var started = false

    private val args by lazy {
        Shizuku.UserServiceArgs(ComponentName(ctx.packageName, PrivilegedService::class.java.name))
            .daemon(false).processNameSuffix("privileged").debuggable(false).version(5)
    }

    private val conn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder?) {
            if (binder == null || !binder.pingBinder()) { _state.value = ShizukuState.ERROR; return }
            val s = IPrivilegedService.Stub.asInterface(binder)
            service = s; backend = ShizukuBackend(s)
            _state.value = ShizukuState.CONNECTED
            PlasmaLog.i("shizuku", "serviço privilegiado conectado, uid=${runCatching { s.uid() }.getOrNull()}")
        }
        override fun onServiceDisconnected(name: ComponentName) {
            service = null; backend = null; refresh()
        }
    }

    fun start() {
        if (started) return
        started = true
        Shizuku.addBinderReceivedListenerSticky(onBinder)
        Shizuku.addBinderDeadListener(onDead)
        Shizuku.addRequestPermissionResultListener(onPerm)
        refresh()
    }

    fun isInstalled(): Boolean = try {
        ctx.packageManager.getPackageInfo("moe.shizuku.privileged.api", 0); true
    } catch (_: Exception) { false }

    fun requestPermission() {
        if (Shizuku.pingBinder() && !Shizuku.isPreV11() && Shizuku.checkSelfPermission() != PackageManager.PERMISSION_GRANTED)
            Shizuku.requestPermission(1001)
    }

    fun refresh() {
        try {
            if (!Shizuku.pingBinder()) { _state.value = if (isInstalled()) ShizukuState.NOT_RUNNING else ShizukuState.NOT_INSTALLED; return }
            if (Shizuku.isPreV11() || Shizuku.checkSelfPermission() != PackageManager.PERMISSION_GRANTED) {
                _state.value = ShizukuState.NO_PERMISSION; return
            }
            if (service != null) { _state.value = ShizukuState.CONNECTED; return }
            _state.value = ShizukuState.CONNECTING
            Shizuku.bindUserService(args, conn)
        } catch (t: Throwable) {
            PlasmaLog.e("shizuku", "refresh falhou", t); _state.value = ShizukuState.ERROR
        }
    }

    fun backend(): PrivilegedBackend? = backend
}
