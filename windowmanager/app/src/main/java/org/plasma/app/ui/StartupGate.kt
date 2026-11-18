
package org.plasma.app.ui

import android.Manifest
import android.content.Context
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.media.MediaPlayer
import android.view.Surface
import android.view.TextureView
import androidx.compose.ui.viewinterop.AndroidView
import org.plasma.app.R
import kotlin.math.max
import android.content.Intent
import android.content.pm.PackageManager
import android.content.ComponentName
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.Settings
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.*
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import androidx.compose.runtime.snapshotFlow
import kotlinx.coroutines.withTimeoutOrNull
import org.plasma.app.session.DesktopSession
import org.plasma.bridge.ShizukuState

@Composable
fun StartupGate(
    session: DesktopSession,
    gpuReady: Boolean,
    onRequestStorage: () -> Unit,
    onOpenNotificationSettings: () -> Unit,
) {
    val app = session.app
    val context = LocalContext.current
    val appsReady by app.appsReady.collectAsState()
    val shizuku by app.shizuku.state.collectAsState()
    val gpuStatus = rememberUpdatedState(gpuReady)
    val appsStatus = rememberUpdatedState(appsReady)
    val prefs = remember(context) { context.getSharedPreferences("plasma_first_run", 0) }

    var showBoot by remember { mutableStateOf(true) }
    SideEffect { session.requestStorage = onRequestStorage; session.openNotificationSettings = onOpenNotificationSettings }

    LaunchedEffect(Unit) {
        delay(1150)
        val ready = withTimeoutOrNull(6500) {
            snapshotFlow { gpuStatus.value && appsStatus.value }.first { it }
        }
        if (ready == true) delay(180)
        showBoot = false

        if (session.displayId == 0 && (!prefs.getBoolean("completed", false) || !app.debian.ready)) session.openTool("setup")
    }

    Box(Modifier.fillMaxSize()) {
        DesktopRoot(session)
        if (showBoot) {
            BootScreen(gpuReady = gpuReady, appsReady = appsReady)
        }
    }
}

@Composable
private fun BootScreen(gpuReady: Boolean, appsReady: Boolean) {
    Box(
        Modifier.fillMaxSize().background(Color.Black)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
    ) {
        BootVideo(Modifier.fillMaxSize())
    }
}

@Composable
private fun BootVideo(modifier: Modifier) {
    AndroidView(
        factory = { ctx -> FitVideoView(ctx) },
        modifier = modifier,
    )
}

private class FitVideoView(context: Context) : TextureView(context), TextureView.SurfaceTextureListener {
    private var player: MediaPlayer? = null
    private var surface: Surface? = null
    private var videoW = 0
    private var videoH = 0

    init {
        isOpaque = true
        surfaceTextureListener = this
    }

    override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
        val mp = MediaPlayer()
        try {
            context.resources.openRawResourceFd(R.raw.splash_boot).use { fd ->
                mp.setDataSource(fd.fileDescriptor, fd.startOffset, fd.length)
            }
            val s = Surface(st)
            surface = s
            mp.setSurface(s)
            mp.isLooping = true
            mp.setVolume(0f, 0f)
            mp.setOnVideoSizeChangedListener { _, vw, vh ->
                videoW = vw
                videoH = vh
                applyFit()
            }
            mp.setOnPreparedListener { it.start() }
            mp.prepareAsync()
            player = mp
        } catch (_: Exception) {
            mp.release()
        }
    }

    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) = applyFit()

    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
        player?.release()
        player = null
        surface?.release()
        surface = null
        return true
    }

    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}

    private fun applyFit() {
        val vw = width.toFloat()
        val vh = height.toFloat()
        if (videoW <= 0 || videoH <= 0 || vw <= 0f || vh <= 0f) return
        val scale = max(vw / videoW, vh / videoH)
        val sx = videoW * scale / vw
        val sy = videoH * scale / vh
        setTransform(Matrix().apply { setScale(sx, sy, vw / 2f, vh / 2f) })
    }
}

@Composable
internal fun FirstRunSetup(
    app: org.plasma.app.PlasmaApp,
    shizuku: ShizukuState,
    onRequestStorage: () -> Unit,
    onOpenNotificationSettings: () -> Unit,
    onRefresh: () -> Unit,
    onFinish: () -> Unit,
) {
    val context = LocalContext.current
    val b = LocalBreeze.current
    var storageGranted by remember { mutableStateOf(hasStorageAccess(context)) }
    var notificationGranted by remember { mutableStateOf(hasNotificationListenerAccess(context)) }

    LaunchedEffect(Unit) {
        while (true) {
            storageGranted = hasStorageAccess(context)
            notificationGranted = hasNotificationListenerAccess(context)
            delay(700)
        }
    }

    Box(
        Modifier.fillMaxSize().background(Color(0x99000000))
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
        contentAlignment = Alignment.Center,
    ) {
        PopupSurface(
            Modifier.fillMaxWidth(.92f).widthIn(max = 430.dp).heightIn(max = 560.dp),
            z = 300f,
        ) {
            Column(
                Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(20.dp),
            ) {
                T("Bem-vindo ao Plasma", 21.sp, weight = FontWeight.SemiBold)
                T(
                    "Prepare as permissões para usar janelas Android, arquivos e integração com o sistema. Os recursos opcionais podem ser configurados depois.",
                    12.sp, b.dim, modifier = Modifier.padding(top = 6.dp, bottom = 12.dp),
                )

                SetupPermissionRow(
                    title = "Armazenamento",
                    detail = if (storageGranted) "Acesso a arquivos concedido" else "Acesso para o Dolphin e comandos do terminal",
                    state = if (storageGranted) "Concedido" else "Opcional",
                    granted = storageGranted,
                    actionLabel = if (storageGranted) "OK" else "Configurar",
                    onAction = onRequestStorage,
                )

                SetupPermissionRow(
                    title = "Shizuku",
                    detail = when (shizuku) {
                        ShizukuState.CONNECTED -> "Janelas Android com toque, mouse e teclado"
                        ShizukuState.NO_PERMISSION -> "Autorize o serviço para controlar janelas Android"
                        ShizukuState.NOT_INSTALLED -> "Instale o Shizuku para habilitar janelas interativas"
                        ShizukuState.NOT_RUNNING -> "Inicie o Shizuku antes de abrir aplicativos em janelas"
                        ShizukuState.CONNECTING -> "Conectando ao serviço privilegiado…"
                        ShizukuState.ERROR -> "Não foi possível conectar; tente novamente"
                    },
                    state = shizuku.label,
                    granted = shizuku == ShizukuState.CONNECTED,
                    actionLabel = when (shizuku) {
                        ShizukuState.CONNECTED -> "Conectado"
                        ShizukuState.NO_PERMISSION -> "Permitir"
                        ShizukuState.NOT_INSTALLED -> "Obter"
                        ShizukuState.NOT_RUNNING -> "Abrir"
                        else -> "Reavaliar"
                    },
                    onAction = {
                        when (shizuku) {
                            ShizukuState.NO_PERMISSION -> { app.shizuku.requestPermission(); onRefresh() }
                            ShizukuState.NOT_INSTALLED -> runCatching {
                                context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://shizuku.rikka.app/")))
                            }
                            ShizukuState.NOT_RUNNING -> {
                                val launch = context.packageManager.getLaunchIntentForPackage("moe.shizuku.privileged.api")
                                if (launch != null) context.startActivity(launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                                else onRefresh()
                            }
                            else -> onRefresh()
                        }
                    },
                )

                SetupPermissionRow(
                    title = "Notificações",
                    detail = "Autoriza o Plasma a exibir alertas recebidos na bandeja",
                    state = if (notificationGranted) "Concedido" else "Opcional",
                    granted = notificationGranted,
                    actionLabel = if (notificationGranted) "Ativo" else "Abrir configurações",
                    onAction = onOpenNotificationSettings,
                )

                T(
                    "O desktop e os aplicativos internos funcionam sem essas permissões. Shizuku é necessário para abrir e controlar a maioria dos apps Android dentro das janelas.",
                    11.sp, b.dim, modifier = Modifier.padding(top = 14.dp),
                )
                Row(
                    Modifier.fillMaxWidth().padding(top = 18.dp),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.clickable { onFinish() }.padding(horizontal = 12.dp, vertical = 10.dp)) {
                        T("Agora não", 12.sp, b.dim)
                    }
                    Spacer(Modifier.width(6.dp))
                    BreezeButton("Concluir configuração", onFinish, primary = true)
                }
            }
        }
    }
}

@Composable
internal fun SetupPermissionRow(
    title: String,
    detail: String,
    state: String,
    granted: Boolean,
    actionLabel: String,
    onAction: () -> Unit,
) {
    val b = LocalBreeze.current
    Column(
        Modifier.fillMaxWidth().padding(vertical = 7.dp).clip(RoundedCornerShape(7.dp))
            .background(b.view.copy(alpha = .68f)).border(1.dp, b.line, RoundedCornerShape(7.dp)).padding(11.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            T(title, 13.sp, weight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
            T(state, 10.sp, if (granted) Color(0xFF27AE60) else b.dim)
        }
        T(detail, 11.sp, b.dim, modifier = Modifier.padding(top = 3.dp, bottom = 8.dp))
        BreezeButton(actionLabel, onAction, enabled = !granted, primary = !granted)
    }
}

internal fun hasStorageAccess(context: android.content.Context): Boolean =
    if (Build.VERSION.SDK_INT >= 30) {
        Environment.isExternalStorageManager()
    } else {
        ContextCompat.checkSelfPermission(context, Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
    }

internal fun hasNotificationListenerAccess(context: android.content.Context): Boolean {
    val enabled = Settings.Secure.getString(
        context.contentResolver,
        "enabled_notification_listeners",
    ).orEmpty()
    val component = ComponentName(context, org.plasma.app.android.PlasmaNotificationService::class.java)
    return enabled.split(':').any { ComponentName.unflattenFromString(it) == component }
}
