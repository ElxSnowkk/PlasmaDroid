package org.plasma.bridge;

import android.view.Surface;

// Servico que roda com a identidade "shell" (uid 2000) via Shizuku UserService.
interface IPrivilegedService {
    void destroy() = 16777114;                       // exigido pelo Shizuku
    int uid() = 1;
    int createDisplay(String name, int width, int height, int dpi, in Surface surface, int flags) = 2;
    void releaseDisplay(int displayId) = 3;
    void resizeDisplay(int displayId, int width, int height, int dpi) = 4;
    void setDisplaySurface(int displayId, in Surface surface) = 5;
    String lastError() = 6;
    String exec(in String[] cmd, int timeoutMs) = 7;
    boolean injectMotion(int displayId, int action, float x, float y, long downTime, long eventTime, int source) = 10;
    // Toque/mouse em lote: `times` traz K amostras (histórico + atual), `ids` P ponteiros e `xy` K*P*2 floats
    // (amostra-major, depois ponteiro). Assíncrono e ordenado: não bloqueia quem chama e preserva o histórico,
    // o que mantém o arrastar/fling fluido. `action` já mascarado (0 DOWN, 1 UP, 2 MOVE, 3 CANCEL, 5/6 POINTER_*, 7 HOVER_MOVE).
    oneway void injectTouch(int displayId, int action, int actionIndex, int source, int buttonState, long downTime, in long[] times, in int[] ids, in float[] xy) = 13;
    oneway void injectKey(int displayId, int keyCode, int action, int meta, long downTime, long eventTime) = 11;
    oneway void injectScroll(int displayId, float x, float y, float vScroll, float hScroll, long time) = 12;
    // Bluetooth com identidade shell: "connect" | "disconnect" | "unpair". Devolve "ok" ou "erro: ...".
    // Contadores e último erro da injeção de entrada (diagnóstico).
    String inputStats() = 21;
    String bluetooth(String op, String address, int timeoutMs) = 20;
}
