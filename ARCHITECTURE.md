# Blone — Arquitetura

Blone é uma aplicação Android nativa de modelagem 3D: gratuita, offline, com
interface profissional simples de aprender pelo ecrã tátil, desenhada para
telemóveis com recursos limitados. Este documento descreve a arquitetura
implementada nas **Fases 0, 1 e 2** e as responsabilidades de cada camada.

---

## 1. Visão geral das camadas

```
┌────────────────────────────────────────────────────────────┐
│  Kotlin / Jetpack Compose (UI)                             │
│  MainActivity · BloneOverlay · BloneViewModel              │
│  → modos Objeto/Editar, seleção V/A/F, ferramentas         │
│    explícitas, ações de modelação, desfazer/refazer,       │
│    guardar/abrir (SAF), painel Avançado                    │
├────────────────────────────────────────────────────────────┤
│  Kotlin / Gestos e estado (gl/)                            │
│  BloneSurfaceView (GLSurfaceView + multitouch)             │
│  TouchTracker · CameraState · FpsMeter                     │
│  → lógica pura, sem APIs Android, testável na JVM          │
├────────────────────────────────────────────────────────────┤
│  Ponte JNI (NativeLib ↔ native-lib.cpp)                    │
│  câmara por frame + picking + operações + histórico +      │
│  projeto — um único mutex serializa cena e renderer        │
├────────────────────────────────────────────────────────────┤
│  C++ (app/src/main/cpp)                                    │
│  renderer/Renderer (OpenGL ES 3.0, buffers por objeto,     │
│    overlays de seleção, picking CPU por elemento)          │
│  core/mesh/ HalfEdgeMesh · Primitives                      │
│  core/operations/ Operations · TransformMath               │
│  core/history/History                                      │
│  core/scene/ Scene · SceneOps                              │
│  core/project/Project (formato BLONEPRJ, docs/PROJECT_FORMAT.md) │
│  core/ io/ (vazio na Fase 2; preenchido na Fase 3)         │
│  tests/ suíte do núcleo em host (CMake + ctest, sem Android)│
└────────────────────────────────────────────────────────────┘
```

Regra estrutural: **a interface (Kotlin/Compose) e o núcleo 3D (C++) estão
separados**. A fronteira é a ponte JNI em `NativeLib`. Não existem camadas
genéricas além do que as fases exigem.

## 2. Núcleo geométrico (Fase 2)

### 2.1 Representação topológica — half-edge

`core/mesh/HalfEdgeMesh` é uma DCEL dirigida com:

- **Elementos** `Vertex`, `HalfEdge`, `Edge`, `Face`, cada um com um
  **id persistente** (contadores monótonos por objeto; 0 = nulo). Os ids
  nunca são reutilizados dentro de uma sessão; slots internos são
  reciclados via free-lists (memória), com deteção de ids obsoletos
  (`slot element id == pedido id`).
- **Fronteiras**: um half-edge com `face == 0` é de fronteira; os
  half-edges de fronteira formam ciclos fechados ligados por
  `next`/`prev` (sem face exterior fantasma). Malhas abertas (plano,
  extrusões) são cidadãos de primeira classe.
- **Faces poligonais** (3..256 lados). A triangulação é derivada na
  renderização (fan) — a estrutura editável nunca é "apenas triângulos".
- Índice dirigido `(origem, destino) → half-edge` (hash map) para
  resolução O(1) em `addFace`.

**Invariantes** (verificados por `validate()` e pela suíte de testes, após
cada operação):

1. `he.twin.twin == he`;
2. `next`/`prev` coerentes em todos os loops (face e fronteira);
3. cada half-edge de um loop referencia essa face; `face.he` pertence ao
   próprio loop; half-edges de fronteira têm `face == 0`;
4. cada aresta viva tem 1 ou 2 faces adjacentes e os seus dois half-edges
   nunca pertencem à mesma face (sem arestas "de leque");
5. sem referências pendentes (tudo aponta para elementos vivos ou 0);
6. sem órfãos (vértice vivo ⇒ `outgoing` válido; face/aresta vivas ⇒ `he`
   válido);
7. loops de face com 3..256 vértices distintos.

### 2.2 Primitivas (`core/mesh/Primitives`)

Cubo (6 quads), plano (1 quad com fronteira), cilindro (n lados + 2 tampas)
e esfera UV (anéis de quads + leques triangulares nos polos). Todos geram
`HalfEdgeMesh` válida, constroem numa malha temporária e só depois trocam
(contra estado parcial). Normais para fora verificadas por teste (produto
interno normal·centro > 0).

### 2.3 Operações (`core/operations/Operations`)

Contrato de todas: **validação prévia (só leitura) → mutação → pós-validação
→ reversão em caso de falha** (guard `Rollback` com snapshot interno). Nenhuma
operação deixa a malha parcialmente alterada.

- **Extrusão de faces** (`extrudeFaces`): a região selecionada é removida,
  paredes são criadas ao longo de TODAS as arestas de fronteira da região e
  uma face de topo (cópia da região) nasce em vértices duplicados. A
  fronteira da região é percorrida e validada antes de qualquer mutação;
  regiões fechadas (ex.: o cubo inteiro) são rejeitadas. Vértices da região
  que fiquem isolados (região sobre a fronteira da malha) são recriados uma
  única vez para manter o anel das paredes ligado. A seleção é remapeada
  para as faces de topo.
- **Subdivisão** (`subdivideFaces`): regra linear documentada —
  **quad → 4 quads** (pontos médios das arestas + centroide da face),
  **triângulo → 4 triângulos** (pontos médios). Os pontos médios são
  partilhados entre faces adjacentes (divisão de aresta). Vizinhas fora da
  seleção mantêm a forma (ganham vértices no loop — polígonos válidos,
  p.ex. pentágonos; verificado por teste). A união de arestas a dividir é
  recolhida ANTES de qualquer divisão (evita dividir duas vezes sub-arestas).
  Sem suavização Catmull-Clark nesta fase — comportamento previsível e barato.
- **Apagar** (`deleteElements`): validação prévia de TODA a seleção:
  - vértice: o link (arestas das faces do estrela que não tocam o vértice)
    tem de ser um único ciclo ou um único caminho; caso contrário
    (`ErrNonManifold`) a malha fica intacta;
  - aresta com 2 faces: as faces fundem-se num polígono; exigem partilhar
    EXATAMENTE uma aresta e caber no limite de lados;
  - aresta de fronteira: a única face adjacente é removida com ela;
  - face: removida; arestas sem faces e vértices isolados são limpos.
- **Loop cut** (`loopCut`): definição concreta, fixada por teste no cubo:
  partindo da aresta selecionada (que é atravessada/dividida), o anel entra
  em cada quad adjacente e sai pela aresta **oposta**; o percurso continua
  até fechar (anel fechado) ou atingir uma aresta de fronteira (anel aberto,
  termina aí). Face não-quad no anel, anel que não fecha nem termina em
  fronteira, ou percursos sobrepostos → **rejeição limpa** com a malha
  byte-idêntica ao estado anterior. No cubo (corte pelas 4 arestas
  verticais): V 8→12, E 12→20, F 6→10 (4 faces do anel → 8 quads; topo e
  base mantêm-se quads), Euler 2, novos vértices exatamente nos pontos
  médios. No plano: anel aberto de 1 face, V 4→6, E 4→7, F 1→2, Euler 1.
  Faces atravessadas são divididas em 2 quads ligados por uma aresta nova
  entre os pontos médios; vértices que morrem com a divisão (malha só com
  a face do anel) são recriados uma vez na posição registada.
- **Transformações** (`TransformMath` + sessão de ferramenta em
  `SceneOps`): mapeamentos puros e testados —
  Mover: a seleção segue o dedo (`right·dx + up·(-dy)` × unidades/px à
  distância da câmara); Rodar: arrasto horizontal = rotação em torno do Y
  do mundo (-0,4°/px), vertical = em torno do right da câmara (+0,4°/px),
  pivô = centroide da seleção; Escalar: fator uniforme
  `clamp(1 - 0,005·dy, 0,05, 20)` em torno do centroide. A sessão
  (início → movimentos → conclusão) aplica os deltas acumulados desde as
  posições iniciais (determinístico, sem deriva) e grava UMA entrada de
  histórico no fim.

### 2.4 Histórico (`core/history/History`) — memória verificável

Estratégia (condição 3 do proprietário):

- Transformações guardam **deltas de posições** (barato e exato);
- Operações topológicas guardam **snapshots antes/depois** da malha;
- Adição/remoção de objetos guarda o objeto inteiro;
- `usedBytes()` é a contagem EXATA de bytes dos envelopes (pools + mapas +
  free-lists); `lastEntryBytes()` expõe o custo da última entrada;
- **Orçamento rígido** (predefinição 24 MB, máx. 64 entradas) com
  reserva em duas fases: `reserve(limite_superior)` corre ANTES de qualquer
  mutação — evicta as entradas mais antigas (e o ramo de refazer) até caber;
  se nem o limite superior couber, a operação é **rejeitada
  (`ErrBudget`) com a malha intacta**. O push substitui o limite pelo custo
  exato. Nova operação após desfazer descarta o ramo de refazer.
- Os multiplicadores de limite por operação estão em `SceneOps.cpp`
  (`kBoundExtrude/Delete/LoopCut/Subdivide`) e são documentados como
  conservadores; o custo real gravado é sempre medido, nunca estimado.

### 2.5 Cena e fachada (`core/scene`)

`Scene` = lista de `SceneObject` (nome, posição, malha, seleção por
conjuntos de ids), objeto ativo, modo de edição, modo de seleção,
multisseleção e um `version` incrementado em qualquer mudança estrutural
(o renderer ressincroniza a partir dele). `SceneOps` é a única fachada
usada pelo JNI: valida → reserva histórico → muta → pós-valida → grava
entrada + remapeia seleção.

### 2.6 Projeto (`core/project`, `docs/PROJECT_FORMAT.md`)

Formato binário próprio **BLONEPRJ v1**: magic, versão, comprimento, CRC32
e payload com geometria + topologia completa (ids persistentes) + nomes +
posições + seleção + estado de edição. Abertura 100% validada em cena
temporária; erro algum destrói o projeto aberto. Ver tabela de erros em
`docs/PROJECT_FORMAT.md`.

## 3. Renderização (`renderer/Renderer`)

- Grelha/eixos estáticos (Fase 1) + **um conjunto de buffers por objeto**:
  triângulos com sombreamento plano (fan + normais por face, duplicação de
  vértices), wireframe de linhas, pontos nos vértices reais.
- Buffers reconstruídos quando `Scene::version` muda (troca conhecida e
  documentada: malhas limitadas ⇒ reconstrução barata e sempre coerente;
  otimizações incrementais só com medições, conforme a regra de
  desempenho).
- Overlays do objeto ativo em modo Editar: faces selecionadas tingidas
  (com `polygonOffset`), arestas selecionadas + contornos de faces
  selecionadas em âmbar, todos os vértices como pontos pequenos e os
  selecionados como pontos maiores. Modo Objeto: contorno âmbar da caixa
  do objeto ativo.
- **Picking CPU** (`pickAtPixel`): raio por pixel com as matrizes do último
  frame; interseção raio-triângulo (Möller–Trumbore) em todos os objetos
  (em espaço local: origem − posição do objeto); fica o hit MAIS PRÓXIMO
  (oclusão respeitada). Vértices/arestas candidatos vêm da face atingida,
  escolhidos por distância em espaço de ecrã (raio ~14 dp) e aceites só se
  não estiverem atrás da superfície visível (comparação de profundidade ao
  longo do raio). Elementos escondidos não são selecionados.

## 4. Kotlin (UI e gestos)

| Ficheiro | Responsabilidade |
|---|---|
| `MainActivity.kt` | Activity única; SAF (`CreateDocument`/`OpenDocument`) para Guardar/Abrir — sem permissões de armazenamento abrangentes; liga o ciclo de vida do Activity à superfície GL. |
| `ui/BloneOverlay.kt` | Chrome Compose: topo (título, FPS, Desfazer/Refazer sempre visíveis, Guardar/Abrir, Avançado); barra de modos (Objeto/Editar ativos; Esculpir/Pintar desativados "em breve"); em Editar: seleção Vértice/Aresta/Face + multisseleção + Limpar, ferramentas Mover/Rodar/Escalar + Concluir, ações Extrudir/Subdividir/Apagar/Loop cut; painel Avançado com primitivas, lista de objetos, apagar objeto e nova cena. Botões ≥ 48 dp. |
| `vm/BloneViewModel.kt` | Estado que sobrevive a rotação; operações nativas em `Dispatchers.Default` (nunca bloqueiam a UI nem a thread GL); mensagens de estado em PT mapeadas dos códigos nativos. |
| `gl/BloneSurfaceView.kt` | GLSurfaceView ES 3.0; prioridade de interação: botões Compose > arrasto da ferramenta armada (1 dedo) > tap de seleção (1 dedo curto) > órbita (1 dedo); **2 dedos é SEMPRE câmara** (pan+zoom), em qualquer modo — nenhum gesto rouba a câmara. |
| `gl/NativeLib.kt` | Declarações JNI + códigos de estado; documenta o contrato de threads. |
| `gl/TouchTracker.kt` / `CameraState.kt` / `FpsMeter.kt` | Inalterados da Fase 1 (testados): máquina multitoque, câmara orbital com clamps, FPS real por timestamps. |

## 5. Limites de geometria e memória

| Limite | Valor |
|---|---|
| Vértices por objeto | 200 000 |
| Arestas por objeto | 400 000 |
| Half-edges por objeto | 800 000 |
| Faces por objeto | 200 000 |
| Lados por face | 256 |
| Objetos por cena | 32 |
| Segmentos U (cilindro/esfera) | 3..96 |
| Bandas V (esfera) | 2..48 |
| Tamanho das primitivas | 0,01..64 unidades |
| Orçamento do histórico | 24 MB / 64 entradas (o que chegar primeiro) |

Verificações de limites (com margem conservadora para o pior caso) correm
ANTES das alocações; extrapolação devolve `ErrLimit`/`ErrBudget` com a malha
intacta. Alocações de contagens grandes são checadas contra overflow
(`size_t`) antes de `reserve`.

Desempenho conhecido e aceite (documentado, sem otimização especulativa):
operações topológicas e sincronização de buffers são O(tamanho da malha);
por isso os limites acima mantêm as malhas práticas pequenas, as operações
correm fora da thread principal e a UI nunca bloqueia.

## 6. Build e CI

- Gradle (Kotlin DSL) + AGP 8.7.3 + Kotlin 2.0.21 (plugin Compose) +
  Compose BOM 2024.12.01; JDK 17; NDK r27 (27.1.12297006) fixado;
  CMake 3.22.1; minSdk 26 / targetSdk 35; ABIs armeabi-v7a, arm64-v8a,
  x86_64; versionName `0.2.0-fase2`.
- `.github/workflows/android.yml` — **condição 7 (testes do núcleo
  primeiro)**:
  1. job `core-tests`: `cmake` + `ctest` da suíte do núcleo C++ em host
     (sem Android); o job do APK depende (`needs`) deste;
  2. job `build`: checkout → JDK 17 → cache Gradle → `assembleDebug` +
     `testDebugUnitTest` (21 testes JVM) → artifacts;
  3. assinatura release: **inalterada em relação às Fases 0/1** — secrets
     `ANDROID_KEYSTORE_*`, keystore descodificado só no job em
     `$RUNNER_TEMP/blone-signing` (chmod 700), removido com `if: always()`,
     `apksigner verify` no runner, artifact publicado; sem secrets, o
     release assinado é omitido com aviso claro (o debug nunca é
     apresentado como release assinado);
  4. `GITHUB_TOKEN` com `permissions: contents: read`; artifacts com nome
     `Blone-fase2-*` + SHA curto; retenção 30 dias; releases públicas só
     com autorização expressa do proprietário.

## 7. Testes

- **Núcleo C++ em host** (`app/src/main/cpp/tests`, CMake + ctest no CI):
  1107 verificações em 5 suítes — invariantes topológicas após cada
  operação; estrutura e normais das 4 primitivas + rejeições de parâmetros;
  extrusão (contagens exatas V12/E20/F10 no cubo, paredes para fora,
  região fechada rejeitada, extrusão do plano); subdivisão (cubo inteiro
  26/48/24; face única com pentágonos de vizinhança); apagar (face, aresta
  interior com fusão em hexágono, vértice interior com buraco, aresta de
  fronteira) e rejeições de não-manifold (vértice bowtie, faces que
  partilham mais do que uma aresta) com a malha intacta; **loop cut com
  definição concreta** no cubo (anel fechado: 12/20/10, pontos médios
  exatos, tudo quads), anel aberto no cubo sem topo (12/19/8, Euler 1) e
  no plano (6/7/2), rejeição com triângulo no anel (malha byte-idêntica);
  mapeamentos de arrasto (seguir o dedo, rotações, escala limitada);
  histórico (rejeição por orçamento antes da mutação, contagem exata de
  bytes, evicção, desfazer/refazer com igualdade exata, invalidação do
  ramo de refazer, sessão de ferramenta com UMA entrada); projeto
  (round-trip byte-exato; rejeição de magic/versão/truncatura/CRC com a
  cena atual intacta); sequência-mestre (criar cubo → selecionar face →
  extrudir → desfazer → refazer → guardar → nova cena → abrir → igual).
- **JVM Kotlin** (CI): 21 testes de câmara, gestos e FPS (Fase 1, mantidos).
- **Dispositivo físico** (responsabilidade do proprietário): sequência-mestre
  no app, gestos, rotação, FPS real no contador. O objetivo de 60 FPS é
  avaliado no dispositivo, nunca prometido por padrão.

## 8. Fases futuras (não implementadas aqui)

Fase 3: tinygltf (glTF 2.0/GLB) com versão fixada e licença verificada,
importador OBJ, exportação GLB/glTF, stb_image para texturas, xatlas para UV.
Nenhuma destas dependências ou funcionalidades foi integrada na Fase 2 —
`core/io/` está vazio de propósito.
