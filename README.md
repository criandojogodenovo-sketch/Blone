# Blone

Aplicação Android nativa de modelagem 3D — gratuita, offline e pensada para
ecrãs táteis e telemóveis com recursos limitados.

**Estado atual: Fase 2 + correção de renderização/interação** (base nativa +
viewport tátil + modelagem editável + gizmo funcional no écran).

## Instalação

1. Descarrega o APK em *Actions → run mais recente → Artifacts*
   (`Blone-fase2-debug-…` ou `Blone-fase2-release-assinado-…`), ou numa
   release do separador *Releases*, quando existir.
2. No telemóvel, permite "Instalar apps de fontes desconhecidas" para este
   ficheiro e instala o APK.
3. Requisitos: Android 8.0 (API 26) ou superior.

## Funcionalidades (Fase 2 + correção)

- Viewport 3D real: renderização C++ com OpenGL ES 3.0 (grelha, eixos
  X/Vermelho · Y/Verde · Z/Azul, malhas com sombreamento plano e wireframe).
- **Cubo correto**: 8 vértices, 6 faces com winding anti-horário (visto de
  fora), 12 triângulos, validação registada no Logcat na criação.
- **Triangulação robusta** (ear clipping): sem laços cruzados em faces
  côncavas; quads triangulados sempre como (a,b,c)+(a,c,d). A geometria
  desenhada e a malha editável derivam da MESMA fonte.
- **Luz de preenchimento**: as faces viradas para longe da luz mantêm
  leitura (culling permanece ativo — sem buracos falsos).
- Primitivas editáveis: Cubo, Esfera, Cilindro e Plano (núcleo half-edge).
- Modo Editar com seleção de vértices, arestas e faces (oclusão respeitada,
  snap por distância ponto-segmento), seleção múltipla opcional.
- **GIZMO no écran**: eixos X/Y/Z para mover, anéis para rodar, eixos +
  centro para escalar — arrastar uma alça transforma mesmo a seleção (ou o
  objeto, se nada estiver selecionado); arrastar FORA do gizmo orbita a
  câmara; **dois dedos é sempre a câmara** e cancela o arrasto do gizmo.
- Extrusão de faces, subdivisão (quads/triângulos), apagar vértices/arestas/
  faces com validação topológica (rejeição sem mutação parcial), loop cut
  em anéis de quads.
- Desfazer/Refazer com orçamento de memória (24 MB) — sempre visíveis.
- Guardar/Abrir projeto próprio (formato BLONEPRJ v1, validado com CRC32 —
  ver docs/PROJECT_FORMAT.md) via SAF, sem permissões de armazenamento.
- Interface compacta: barra contextual (V/A/F + gizmo) e painel secundário
  expansível ("Mais ▾") — nada cobre desnecessariamente a viewport.
- Contador de FPS medido a partir de timestamps reais.
- Modos Esculpir/Pintar visíveis mas desativados (fases futuras).

## Compilar localmente

Requisitos: JDK 17, Android SDK (API 35), NDK 27.1.12297006, CMake 3.22.1.

```bash
./gradlew :app:assembleDebug        # APK debug
./gradlew :app:testDebugUnitTest    # testes unitários
./gradlew :app:assembleRelease      # APK release (assinado se os env vars de assinatura existirem)
```

A assinatura release é lida de variáveis de ambiente (`ANDROID_KEYSTORE_PATH`,
`ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`) —
nunca de ficheiros no repositório. Sem elas, o build release é produzido sem
assinatura.

Ver [ARCHITECTURE.md](ARCHITECTURE.md) para a arquitetura completa.
