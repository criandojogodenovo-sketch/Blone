# Blone

Aplicação Android nativa de modelagem 3D — gratuita, offline e pensada para
ecrãs táteis e telemóveis com recursos limitados.

**Estado atual: Fase 2 + correção de estabilidade** (base nativa + viewport
 tátil + modelagem editável + gizmo funcional no écran + seleção com fonte
única no motor, sem z-fighting, layouts sem cortes nas duas orientações e
viewport centrada na área útil).

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
- Modo Objeto: toque num objeto para o ATIVAR; **gizmo do objeto inteiro**
  (mover/rodar/escalar sem entrar em Edição); **Duplicar objeto** (cópia
  deslocada, desfeita pelo undo); apagar objeto.
- Modo Editar com seleção de vértices, arestas e faces (oclusão respeitada,
  snap por distância ponto-segmento), seleção múltipla opcional. A
  identidade do objeto ativo e a seleção de componentes sobrevivem às
  transições Objeto↔Edição (escondidas, nunca perdidas).
- **GIZMO no écran**: eixos X/Y/Z para mover (com centro visível), anéis
  para rodar, eixos + centro para escalar — arrastar uma alça transforma o
  alvo; **alvos de toque adaptados à densidade (regra dos 44 dp) e o corpo
  do eixo inteiro também agarra**; em Edição, com a seleção VAZIA o gizmo
  desaparece (sem alças invisíveis); arrastar FORA do gizmo orbita a
  câmara; **dois dedos é sempre a câmara** e cancela o arrasto do gizmo;
  a ferramenta "Sel." desliga o gizmo (toque = só selecionar).
- **Painel de propriedades** (recolhível): nome real do motor ("Cubo 1"),
  contagens V/A/F, centro do objeto e transformações por passos fixos
  (mover ±0.25, rodar ±15°, escalar ×1.25/÷1.25) com histórico —
  acessibilidade sem arrastos precisos.
- Extrusão de faces, subdivisão (quads/triângulos), apagar vértices/arestas/
  faces com validação topológica (rejeição sem mutação parcial), loop cut
  em anéis de quads.
- Desfazer/Refazer com orçamento de memória (24 MB) — sempre visíveis; o
  desfazer/refazer restaura também um estado de seleção válido.
- Escalas seguras: fatores não finitos ou zero são rejeitados; o fator
  acumulado nunca achata a malha (mínimo 0.01).
- Guardar/Abrir projeto próprio (formato BLONEPRJ v1, validado com CRC32 —
  ver docs/PROJECT_FORMAT.md) via SAF, sem permissões de armazenamento.
- Interface em QUATRO ZONAS nas duas orientações (barra inferior deixa de
  ser o lar das ferramentas): **barra superior compacta** com uma linha
  (undo/refazer, Guardar/Abrir, modo atual, FPS, Avançado, Propriedades);
  **barra lateral de ferramentas** à esquerda (Sel./Mover/Rodar/Escalar
  mais as operações contextuais de cada modo); **painel de propriedades**
  à direita; **barra inferior compacta** (Objeto/Edição + V/A/F + estado
  da seleção). O modelo é desenhado, centrado e tocado no retângulo
  realmente visível (viewport útil com os 4 lados reportada ao renderer);
  painéis limitados com scroll interno em ecrãs pequenos.
- Contador de FPS medido a partir de timestamps reais.
- Esculpir/Pintar continuam indisponíveis (fases futuras) e deixaram de
  ter botões na interface — documentados aqui e no ARCHITECTURE.md.

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
