# Blone

Aplicação Android nativa de modelagem 3D — gratuita, offline e pensada para
ecrãs táteis e telemóveis com recursos limitados.

**Estado atual: Fases 0 e 1 concluídas** (base nativa + viewport tátil).

## Instalação

1. Descarrega o APK em *Actions → run mais recente → Artifacts*
   (`Blone-fase0-1-debug-…` ou `Blone-fase0-1-release-assinado-…`), ou numa
   release do separador *Releases*, quando existir.
2. No telemóvel, permite "Instalar apps de fontes desconhecidas" para este
   ficheiro e instala o APK.
3. Requisitos: Android 8.0 (API 26) ou superior.

## Funcionalidades (Fase 1)

- Viewport 3D real: renderização C++ com OpenGL ES 3.0 (cubo iluminado sobre
  grelha, eixos X/Vermelho · Y/Verde · Z/Azul).
- Um dedo: orbitar. Dois dedos: mover (pan) e zoom por pinça. Toque curto:
  selecionar o cubo (contorno âmbar).
- Contador de FPS medido a partir de timestamps reais dos frames.
- Modos Editar/Esculpir/Pintar visíveis mas desativados (chegam em fases
  futuras).

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
