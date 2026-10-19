# Blone — Arquitetura

Blone é uma aplicação Android nativa de modelagem 3D: gratuita, offline, com
interface profissional simples de aprender pelo ecrã tátil, desenhada para
telemóveis com recursos limitados. Este documento descreve a arquitetura
implementada nas **Fases 0 e 1** e as responsabilidades de cada camada.

---

## 1. Visão geral das camadas

```
┌────────────────────────────────────────────────────┐
│  Kotlin / Jetpack Compose (UI)                     │
│  MainActivity · BloneOverlay · BloneViewModel      │
│  → chrome mínimo sobre o viewport (título, FPS,    │
│    barra de modos, mensagens de estado)            │
├────────────────────────────────────────────────────┤
│  Kotlin / Gestos e estado (gl/)                    │
│  BloneSurfaceView (GLSurfaceView + multitouch)     │
│  TouchTracker · CameraState · FpsMeter             │
│  → lógica pura, sem APIs Android, testável na JVM  │
├────────────────────────────────────────────────────┤
│  Ponte JNI (NativeLib ↔ native-lib.cpp)            │
│  parâmetros de câmara por frame + eventos pick     │
├────────────────────────────────────────────────────┤
│  C++ (app/src/main/cpp)                            │
│  renderer/Renderer (OpenGL ES 3.0, shaders, VAOs)  │
│  core/ Math3D · Camera · Mesh · Scene              │
└────────────────────────────────────────────────────┘
```

Regra estrutural: **a interface (Kotlin/Compose) e o núcleo 3D (C++) estão
separados**. A única fronteira entre os dois mundos é a ponte JNI em
`NativeLib`. Não existem abstrações genéricas nem sistemas preparados para
fases futuras — apenas o que a Fase 0 e a Fase 1 exigem.

## 2. Fluxo de execução (render loop)

1. `BloneSurfaceView` (extensão de `GLSurfaceView`) cria um contexto
   **OpenGL ES 3.0** real (`setEGLContextClientVersion(3)`), com config
   RGBA8888 + depth 24 bits, e `RENDERMODE_CONTINUOUSLY` (necessário para
   medir FPS real).
2. Em cada callback do `Renderer` (thread de render), a bridge chama o C++:
   - `onSurfaceCreated` → `NativeLib.surfaceCreated()` — compila shaders,
     cria VAO/VBO/EBO e geometria estática (cubo, grelha, eixos, arestas de
     seleção);
   - `onSurfaceChanged` → `NativeLib.surfaceChanged(w, h)` — viewport,
     aspeto e reinício das medições de FPS;
   - `onDrawFrame` → `NativeLib.drawFrame(azimuth, elevation, distance,
     tx, ty, tz, selected)` — a câmara orbita é enviada por valor em cada
     frame (poucos `float`s, zero alocações), o C++ reconstrói as matrizes
     view/projeção e desenha a cena.
3. Toques: `onTouchEvent` traduz `MotionEvent` em chamadas ao
   `TouchTracker` (classe pura). Os gestos atualizam o `CameraState` no
   `BloneViewModel` (post para a UI thread); o próximo frame lê o estado
   atualizado.
4. Toque simples (tap): `GLSurfaceView.queueEvent` executa
   `NativeLib.pick(x, y)` na thread de GL — interseção raio/AABB em CPU com
   a snapshot da câmara do último frame — e publica o resultado no
   ViewModel.

## 3. Responsabilidades por ficheiro

### Kotlin (`app/src/main/java/com/blone/app/`)

| Ficheiro | Responsabilidade |
|---|---|
| `MainActivity.kt` | Activity única; hospeda Compose + GLSurfaceView; liga o ciclo de vida do Activity ao da superfície GL (`onPause`/`onResume`). |
| `ui/BloneOverlay.kt` | Chrome Compose: título, FPS, barra de modos (Objeto ativo; Editar/Esculpir/Pintar visíveis mas **desativados**, com etiqueta "em breve"), linha de estado. Botões ≥ 48 dp. |
| `vm/BloneViewModel.kt` | Estado que sobrevive a rotação: câmara, seleção, modo, texto de FPS/estado. |
| `gl/BloneSurfaceView.kt` | GLSurfaceView ES 3.0; tradução de `MotionEvent` → `TouchTracker`; ponte de picking via `queueEvent`; medição real de FPS com `System.nanoTime()` e publicação ~2×/s. |
| `gl/NativeLib.kt` | Declarações JNI + `System.loadLibrary("blone")`. Documenta o contrato de threads. |
| `gl/CameraState.kt` | Câmara orbital pura: órbita (1 dedo), pan (centroide de 2 dedos, sensibilidade ancorada à altura do ecrã), zoom por rácio de distância entre dedos; clamps de elevação/distância/alvo. |
| `gl/TouchTracker.kt` | Máquina de estados multitoque: tap (curto, abaixo do slop, nunca multitoque), órbita, pan+zoom simultâneos, re-ancoragem quando dedos entram/saem (sem saltos nem gestos presos), `onCancel`. |
| `gl/FpsMeter.kt` | FPS real: janela deslizante de 60 timestamps; média derivada do intervalo real; deteta o pior intervalo entre frames; devolve `null` quando há menos de 2 medições (nunca inventa valores). |

### C++ (`app/src/main/cpp/`)

| Ficheiro | Responsabilidade |
|---|---|
| `native-lib.cpp` | Funções JNI; clamps defensivos dos parâmetros da câmara. |
| `renderer/Renderer.h/.cpp` | Programas GL (cubo iluminado; linhas com cor por vértice ou cor uniforme), VAO/VBO/EBO, ordem de desenho (cubo opaco → linhas com blending e depth-mask off → contorno de seleção), snapshot de câmara para picking. Reconstrói todos os objetos GL se `init()` for chamado de novo (perda de contexto). |
| `renderer/Shaders` (inline em `Renderer.cpp`) | `#version 300 es`. Cubo: Lambert + ambiente + Blinn-Phong ligeiro, tint de seleção. Linhas: cor por vértice com blending, opção de override (contorno âmbar). |
| `core/Math3D.h` | Vec3/Mat4 column-major mínimos (perspetiva, lookAt, produto) — sem GLM. |
| `core/Camera.h` | Câmara orbital: direção/olho/bases/view matrix a partir de azimute/elevação/distância/alvo. |
| `core/Mesh.h/.cpp` | Cubo (24 vértices com normais por face, 36 índices), AABB para picking. Cubo de lado 2 centrado em (0, 1, 0) — assente na grelha. |
| `core/Scene.h/.cpp` | Grelha XZ (31+31 linhas, passo 1), eixos X/Vermelho · Y/Verde · Z/Azul (metade positiva opaca, negativa esbatida), interseção raio-AABB (slab test). |

## 4. Decisões e invariantes

- **Offline total**: sem permissão `INTERNET`, sem telemetria, sem anúncios,
  sem contas, sem serviços online.
- **minSdk 26 / targetSdk 35**; ABIs: `armeabi-v7a`, `arm64-v8a`, `x86_64`.
- **Sem alocações por frame**: geometria é estática (`GL_STATIC_DRAW`); a
  câmara passa por valor via JNI; as únicas strings por frame são as do FPS
  (2×/s, thread de render → UI).
- **Ciclo de vida GL**: `GLSurfaceView` gere EGL/contexto;
  `preserveEGLContextOnPause = true` evita recriações desnecessárias; se o
  contexto se perder, `surfaceCreated()` reconstrói tudo. `onPause/onResume`
  do Activity são reencaminhados para a superfície.
- **Rotação**: o Activity trata `orientation|screenSize` sem recriar
  (`configChanges`), a superfície é redimensionada (`surfaceChanged` ajusta
  viewport/aspeto) e o `ViewModel` preserva a pose da câmara — retrato e
  paisagem mantêm grelha e cores dos eixos coerentes.
- **Modos**: só **Objeto** está funcional (seleção por tap). Editar,
  Esculpir e Pintar estão visíveis mas desativados com etiqueta "em breve" —
  nunca sugerem funcionalidade inexistente.
- **Honestidade do FPS**: valores derivados exclusivamente de timestamps
  reais dos frames; antes de 2 frames medidos mostra "—".

## 5. Build e CI

- Gradle (Kotlin DSL) + AGP 8.7.3 + Kotlin 2.0.21 (plugin Compose) +
  Compose BOM 2024.12.01; JDK 17; NDK r27 (27.1.12297006) fixado;
  CMake 3.22.1.
- `.github/workflows/android.yml`:
  1. checkout → JDK 17 → cache Gradle;
  2. `assembleDebug` + `testDebugUnitTest` (falha o job se falhar);
  3. upload do APK **debug** como artifact (nome inclui fase + SHA curto,
     retenção 30 dias);
  4. assinatura release: keystore descodificado dos secrets apenas para o
     job, em `$RUNNER_TEMP/blone-signing` (chmod 700), removido com
     `if: always()`;
  5. `assembleRelease` + verificação da assinatura com `apksigner verify`;
  6. upload do APK **release assinado** como artifact; se os secrets não
     existirem, o job publica apenas o debug e assinala claramente a
     omissão (nunca apresenta debug como release assinado).
- `GITHUB_TOKEN` com `permissions: contents: read` (mínimo necessário;
  artifacts não exigem mais). Releases públicas só por autorização
  explícita do proprietário.

## 6. Assinatura (segurança)

- Segredos vivem exclusivamente em GitHub Actions Secrets:
  `ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD`,
  `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`.
- Nenhum segredo entra no código, no Gradle, no git ou nos logs.
- O keystore é criado e guardado pelo proprietário do projeto; se for
  perdido, instalações existentes assinadas com essa chave não podem ser
  atualizadas normalmente pela mesma aplicação (conflito de assinatura).

## 7. Testes

- **Unitários (JVM)**: `CameraStateTest`, `TouchTrackerTest`, `FpsMeterTest`
  — clamps, direções de pan/órbita, deteção de tap vs. drag, re-ancoragem
  multitoque, rácios de zoom, medição de FPS a partir de timestamps reais.
  Correm em CI (`testDebugUnitTest`).
- **Compilação local/CI**: `assembleDebug`/`assembleRelease`.
- **Dispositivo físico** (responsabilidade do proprietário): instalar o APK,
  verificar gestos (órbita/pan/zoom/tap), rotação, e reportar FPS medidos no
  contador do app — o objetivo de 60 FPS é avaliado no dispositivo, nunca
  prometido por padrão.

## 8. Fases futuras (não implementadas aqui)

tinygltf (importação glTF), xatlas (UV), stb_image (texturas), modos
Editar/Esculpir/Pintar com ferramentas reais, exportação. Nenhuma destas
dependências ou funcionalidades foi integrada nesta fase, conforme a regra
de não criar aparência de progresso.
