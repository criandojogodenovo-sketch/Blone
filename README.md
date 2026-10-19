# Blone

Aplicação Android nativa de modelagem 3D — gratuita, offline e pensada para
ecrãs táteis e telemóveis com recursos limitados.

**Estado atual: Fase 2 concluída** (base nativa + viewport tátil + modelagem de malha editável).

## Instalação

1. Descarrega o APK em *Actions → run mais recente → Artifacts*
   (`Blone-fase0-1-debug-…` ou `Blone-fase0-1-release-assinado-…`), ou numa
   release do separador *Releases*, quando existir.
2. No telemóvel, permite "Instalar apps de fontes desconhecidas" para este
   ficheiro e instala o APK.
3. Requisitos: Android 8.0 (API 26) ou superior.

## Funcionalidades (Fase 2)

- Viewport 3D real: renderização C++ com OpenGL ES 3.0 (grelha, eixos
  X/Vermelho · Y/Verde · Z/Azul, malhas com sombreamento plano e wireframe).
- Primitivas editáveis: Cubo, Esfera, Cilindro e Plano (núcleo half-edge com
  topologia consistente).
- Modo Editar com seleção de vértices, arestas e faces (respeita oclusão),
  seleção múltipla opcional.
- Ferramentas explícitas Mover / Rodar / Escalar (arrasto de 1 dedo com a
  ferramenta armada; **dois dedos é sempre a câmara**).
- Extrusão de faces, subdivisão (quads/triângulos), apagar vértices/arestas/
  faces com validação topológica, loop cut em anéis de quads.
- Desfazer/Refazer com orçamento de memória (24 MB) — sempre visíveis.
- Guardar/Abrir projeto próprio (formato BLONEPRJ v1, validado com CRC32 —
  ver docs/PROJECT_FORMAT.md) via SAF, sem permissões de armazenamento.
- Um dedo: orbitar. Dois dedos: mover (pan) e zoom por pinça. Contador de
  FPS medido a partir de timestamps reais.
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
