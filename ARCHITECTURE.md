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
  renderização e no picking pela MESMA função (`renderdata::faceTriangles`,
  *ear clipping* preservando o winding; ver §3) — a estrutura editável
  nunca é "apenas triângulos".
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
- **Transformações** (gizmo, `TransformMath` + sessão em `SceneOps`):
  correção Fase 2 — as transformações são dirigidas pelo GIZMO no écran
  (eixos/anéis desenhados no pivô da seleção, hit-test em espaço de ecrã):
  - **Mover** (arrasto de ponta de eixo X/Y/Z): deslocamento ao longo do
    eixo do mundo; o eixo é projetado por evento (`GizmoMapping`: direção
    em px, px por unidade de mundo) e o arrasto é projetado nessa direção;
  - **Rodar** (arrasto de anel X/Y/Z): ângulo varrido pelo dedo em torno do
    pivô projetado (`ringSweepDeg`); o sinal depende do eixo apontar para
    ou contra a câmara (`ringSign`, fixado por teste — o ponto sob o dedo
    segue o dedo);
  - **Escalar**: arrasto de eixo = fator axial (só o componente ao longo
    do eixo, em torno do pivô); arrasto do centro = fator uniforme
    `clamp(1 - 0,005·dy, 0,05, 20)`;
  - seleção vazia ⇒ o alvo é o objeto inteiro (o modo Editar nunca
    "bloqueia"); a sessão aplica transformações ACUMULADAS desde as
    posições iniciais (determinístico, sem deriva) e grava UMA entrada de
    histórico no fim; 2 dedos durante um arrasto cancelam a sessão e
    assumem a câmara.

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

**Seleção — fonte única de verdade (correção de estabilidade):** a
seleção vive APENAS no C++. `hasValidSelection()` distingue seleção
vazia/inválida de válida (pelo menos um elemento vivo); `selectionCounts()`
devolve contagens atômicas de elementos VIVOS; `pruneAllSelections()` é
higiene defensiva executada pelo JNI após toda mutação. O Kotlin consome
o snapshot atômico `NativeLib.selectionSnapshot()`
(`[editMode, mode, multi, hasActive, nV, nE, nF, hasValid]`) e apenas
APRESENTA o estado do motor — não mantém cópia própria. O ÍNDICE do
objeto ativo também é lido do motor em cada `refresh()` via
`NativeLib.getSelectedId()` (semântica de ÍNDICE: 0 é o primeiro objeto
válido, -1 = nenhum — nunca validar com `> 0`; não é o id persistente
de elemento). Correção do bug "Properties mostra Nenhum objeto": o
toque no viewport muda `Scene::active` no motor, e o espelho Kotlin
dependia de uma cópia local nunca sincronizada — agora `refresh()`
relê SEMPRE o índice do motor e o painel Propriedades re-le-o ao
abrir (`setPropertiesPanel(true)`). As entradas
`MeshSwap` do histórico guardam a seleção pré/pós da operação
(`pushMeshSwapWithSelection`) e o undo/refazer RESTAURA uma seleção
válida (poda aplicada de seguida). Escalas com fatores não finitos ou ≤ 0
são rejeitadas antes de qualquer mutação; o fator acumulado fica preso a
`[0.01, 100]` (mínimo positivo 0.01 — malha nunca achatada até zero).
Transformações só são gravadas no histórico com geometria finita
(verificação defensiva no fim da sessão).

**Modo Objeto transformável (reorganização da interface):**
`beginTool` funciona nos DOIS modos — no Modo Objeto o alvo é SEMPRE o
objeto ativo inteiro (mover/rodar/escalar sem entrar em Edição); no
Modo Edição o alvo é a seleção válida, com o objeto inteiro como
fallback documentado quando a seleção está vazia. A ferramenta
"Seleção" (`GizmoTool::None`) desliga o gizmo: toques selecionam, nada
transforma, nenhum hit é aceito.

**Duplicar objeto:** `duplicateActiveObject` clona a malha inteira do
objeto ativo (`captureRaw`/`restoreRaw`), SEM copiar a seleção, com
deslocamento visível (+1.5 X), ativa a cópia e grava uma entrada
`ObjAdd` — o undo remove a cópia e restaura o objeto ativo anterior; o
redo re-ativa o objeto re-adicionado (identidade consistente — correção
no `History::redo`, que antes não restaurava o ativo).

**Transformações por passos (painel de propriedades):**
`transformDirect(tool, axis, amount)` aplica passos fixos (mover
±0.25 un., rodar ±15°, escalar ×1.25/÷1.25 — uniforme com `axis==3`,
axial com 0..2) reutilizando os helpers `TransformMath` — nenhuma
matemática nova; valida finitude ANTES de mutar, prende o fator a
[0.01, 100] e grava UMA entrada `verticesDelta`. No Modo Edição o alvo
é a seleção válida (não o objeto inteiro).

**Correção do orçamento do histórico (bug real da Fase 2):**
`History::reserve` subtraía duas vezes os bytes do ramo de refazer
(o `undo()` já os tinha subtraído) — um underflow uint64 fazia
TODA operação seguinte a um desfazer falhar com `ErrBudget`
("limite de histórico excedido") até a app ser reiniciada. Corrigido:
`used_` contabiliza apenas a pilha de desfazer; limpar o ramo de
refazer não subtrai nada. Fixado pela suíte `test_ui_modes.cpp`.

### 2.6 Projeto (`core/project`, `docs/PROJECT_FORMAT.md`)

Formato binário próprio **BLONEPRJ v1**: magic, versão, comprimento, CRC32
e payload com geometria + topologia completa (ids persistentes) + nomes +
posições + seleção + estado de edição. Abertura 100% validada em cena
temporária; erro algum destrói o projeto aberto. Ver tabela de erros em
`docs/PROJECT_FORMAT.md`.

## 3. Renderização (`renderer/Renderer`)

- **Fonte geométrica única** (`core/render/RenderData`): a geometria
  desenhada, a malha editável e o picking derivam da MESMA função —
  `faceTriangles()` triangula cada face com **ear clipping** (robusto para
  polígonos CÔNCAVOS; o fan antigo criava laços cruzados — "setas/Z" —
  após fusões de arestas), preservando o winding; para quads o contrato
  `[a,b,c,d] → (a,b,c)+(a,c,d)` é exato e fixado por teste.
- Sombreamento plano: vértices DUPLICADOS por face (buffer de render ≠
  vértices editáveis; 24 vs 8 no cubo), normal por face recalculada
  (Newell) a cada reconstrução. O shader tem luz principal + **luz de
  preenchimento oposta** (correção "faces invisíveis": as faces viradas
  para longe da luz mantêm leitura — sem desativar o culling).
- **Política de profundidade por passe (correção de estabilidade —
  z-fighting das arestas sobre a superfície):** o passe SÓLIDO é
  empurrado para trás com `GL_POLYGON_OFFSET_FILL (1,1)`; wireframe e
  pontos editáveis rasterizam à profundidade exata da superfície e ganham
  o depth test SEM cintilação; o passe de tint das faces selecionadas usa
  offset `(-1,-1)` com `glDepthMask(GL_FALSE)` (não contamina os passes
  seguintes); arestas/pontos selecionados e marcadores passam o depth
  test na profundidade nativa (ficam corretamente OCULTOS pela geometria
  à frente). `GL_DEPTH_TEST` nunca é desativado globalmente; o estado GL
  é restaurado no fim de CADA passe. O gizmo (UI de ecrã) continua com
  depth test off temporário e restauro imediato.
- **Viewport ÚTIL (correção de estabilidade + reorganização):**
  `setViewportInsetsRect(top, bottom, left, right)` reserva as bandas do
  chrome Compose (barra superior/inferior, barra lateral de ferramentas e
  painel de propriedades) — `glViewport(left, bottom, w-l-r, h-t-b)`,
  aspeto e projeção calculados na área VISIBLE, modelo centrado nela em
  qualquer estado dos painéis. `setViewportInsets(top, bottom)` mantém-se
  como compatibilidade (delega no rect completo). `projectToScreenPx`/
  `pickAtPixel` e o mapeamento do gizmo no JNI usam o MESMO retângulo (o
  que se toca é o que se vê); área degenerada recai para a superfície
  completa. FOV vertical 55° (banda exigida 50–60°), near/far 0.1/200
  coerentes com a distância orbital limitada (2.5–40).
  **Correção "viewport encolhe ao fechar Propriedades":** o inset direito
  era medido apenas enquanto o painel existia na composição — ao fechar,
  o callback de medição desaparecia com ele e o último valor ficava
  preso (viewport estreita persistente). Agora: abertura/fecho passam
  pela função ÚNICA `ViewModel.setPropertiesPanel()` e o overlay repõe
  explicitamente o inset direito a 0 quando o painel fecha (o retângulo
  completo é re-aplicado na thread GL via `queueEvent` + requestRender).
  Nenhum `glScissor` em todo o renderer — nada além do `glViewport`
  limita o desenho.
- Grelha/eixos estáticos + um conjunto de buffers por objeto (fill,
  wireframe, pontos) + contorno do modo Objeto EM CACHE (sem alocação
  por frame). Buffers reconstruídos quando `Scene::version` muda (troca
  documentada: malhas limitadas ⇒ reconstrução barata e coerente).
- Overlays do objeto ativo em Editar: faces selecionadas tingidas (offset
  negativo + depth write off), arestas e pontos selecionados na
  profundidade nativa contra o passe sólido recuado — sem z-fighting e
  com oclusão correta (ver "Política de profundidade por passe").
- **GIZMO** (nos DOIS modos): desenhado por cima (depth test off) no
  pivô da regra PARTILHADA `core/scene/GizmoPivot.h` — a mesma função
  inline alimenta o desenho, o hit-test e o mapeamento JNI de arrasto
  (nenhuma duplicação, nenhuma divergência possível). No Modo Objeto o
  pivô é o centróide do objeto inteiro; em Edição, o da seleção válida.
  Setas X/Y/Z (Mover, com PONTO CENTRAL visível), anéis (Rodar), eixos +
  centro (Escalar), tamanho constante no ecrã. **Ids de alças DISJUNTOS
  por ferramenta** (correção "Escalar: seleção inválida"): cada id mapeia
  exatamente UM triplo (ferramenta, operação, eixo) em `beginGizmoDrag` —
  1..3 pontas/corpo Mover X/Y/Z; 4 centro Escalar uniforme; 11..13 anéis
  Rodar X/Y/Z; 21..23 eixos Escalar X/Y/Z (escala axial). Antes, Escalar
  devolvia 1..3 como o Mover e o arrasto era rejeitado com
  `ErrInvalidArg`. **Alvos de toque adaptados
  à densidade (DECISÃO D):** o Kotlin passa
  `NativeLib.setGizmoTouchScale(density)` uma vez por processo; os raios
  de hit escalam 1:1 (ponta 22 px base ⇒ 44 px de diâmetro a densidade 1
  — regra dos 44 dp; anel 18 px; centro 20 px; CORPO do eixo 10 px por
  ponto-segmento — o toque em qualquer ponto da seta agarra esse eixo),
  os comprimentos visuais escalam moderadamente (teto 1.6×) para não
  explodir em tablets. Hit-test CPU (`gizmoHitAt`) com o MESMO gate do
  desenho: Modo Objeto com objeto ativo, ou Edição com seleção VÁLIDA
  (`hasValidSelection`); `GizmoTool::None` nunca desenha nem produz hit
  — nunca há alças invisíveis ou "fantasma".
- **Picking CPU** (`pickAtPixel`): raio por pixel com as matrizes do
  último frame; interseção raio-triângulo (Möller–Trumbore) sobre a
  MESMA triangulação do render; hit mais próximo (oclusão). Vértices e
  arestas candidatos são procurados em TODOS os elementos vivos do
  objeto atingido (correção: antes só os da face tocada), arestas por
  distância ponto-segmento no ecrã (correção: antes só o ponto médio),
  com verificação de profundidade ao longo do raio — elementos
  escondidos não são selecionados.

## 4. Kotlin (UI e gestos)

| Ficheiro | Responsabilidade |
|---|---|
| `MainActivity.kt` | Activity única; SAF (`CreateDocument`/`OpenDocument`) para Guardar/Abrir — sem permissões de armazenamento abrangentes; liga o ciclo de vida do Activity à superfície GL; encaminha o RETÂNGULO medido do chrome (topo/fundo/esquerda/direita, via `queueEvent` — sempre na thread GL) para a viewport útil GL. |
| `ui/BloneOverlay.kt` | Chrome Compose em QUATRO zonas (DECISÃO B): **A** barra superior compacta (1 linha com scroll — undo/refazer, Guardar/Abrir, chip do modo atual, FPS, Avançado e Propriedades); **B** barra lateral de ferramentas à esquerda (Sel./Mover/Rodar/Escalar nos dois modos + Duplicar/Apagar objeto no Objeto + Extrudir/Subdiv./Apagar/Loop/Limpar/Multi na Edição) — a barra inferior deixa de ser o lar das ferramentas; **C** painel de propriedades contextual à direita, recolhível (leitura do motor: nome, V/A/F, centro; passos fixos de mover/rodar/escalar; ajuda) — abrir/fechar NÃO toca na cena, só muda insets; **D** barra inferior compacta (alternância Objeto/Edição, V/A/F em Edição, estado da seleção reportado pelo motor). Esculpir/Pintar deixaram de ter botões mortos — documentados como indisponíveis. As medidas das 4 zonas são reportadas ao renderer (viewport útil com 4 lados); caps de altura (35% paisagem / 50% retrato) com scroll interno. |
| `vm/BloneViewModel.kt` | Estado que sobrevive a rotação; operações nativas em `Dispatchers.Default` (nunca bloqueiam a UI nem a thread GL); mensagens de estado em PT mapeadas dos códigos nativos; o espelho da seleção e a leitura de propriedades (`propLabel/propVerts/propEdges/propFaces/propCenter`) são preenchidos EXCLUSIVAMENTE por chamadas nativas — nunca derivados no Kotlin; o índice do objeto ativo é re-lido do motor (`getSelectedId`) em cada `refresh()`. `chooseGizmoTool` aceita `Tool.NONE` (gizmo desligado); `duplicateActiveObject` e `nudgeMove/nudgeRotate/nudgeScale` (passos) chamam o motor; `setPropertiesPanel` é o ponto ÚNICO de abertura/fecho do painel (ao abrir, re-le o motor antes de mostrar). |
| `gl/BloneSurfaceView.kt` | GLSurfaceView ES 3.0; prioridade: botões Compose > **arrasto de alça do gizmo** (1 dedo em alça, sem gate de modo — o MOTOR decide nos dois modos) > órbita (1 dedo fora do gizmo) > tap de seleção (1 dedo curto); **2 dedos é SEMPRE câmara** (pan+zoom) e cancela qualquer arrasto de gizmo. `setGizmoTouchScale(density)` uma vez por processo (alvos de toque em dp — DECISÃO D). `preserveEGLContextOnPause = true` (quando o Android PRESERVA o contexto em pausa/rotação); quando o Android o RECRIA (o que não é garantido — depende do driver/dispositivo), `surfaceCreated → Renderer::init()` reconstrói TODOS os recursos GPU a partir da cena CPU (singletons nativos + ViewModel sobrevivem à recriação do Activity no mesmo processo) e `sceneVersion_ = 0` força a ressincronização completa — os dados CPU da cena nunca se duplicam nem se perdem. Todas as chamadas GL correm na thread GL; `setViewportInsetsRect` é aplicado via `queueEvent`. |
| `gl/NativeLib.kt` | Declarações JNI + códigos de estado; documenta o contrato de threads. |
| `gl/TouchTracker.kt` / `CameraState.kt` / `FpsMeter.kt` | TouchTracker com modo GIZMO (alça detetada no ACTION_DOWN por callback; arrasto vai ao gizmo; 2.º dedo cancela e assume a câmara; tap em alça não seleciona) — 100% JVM-testável; câmara orbital com clamps; FPS real por timestamps. |

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
  x86_64; versionName `0.3.0-fase2` (versionCode 5).
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
  1887 verificações em 7 suítes — invariantes topológicas após cada
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
  **Suíte nova da correção** (`test_render_gizmo.cpp`): contagens
  8V/12E/6F do cubo, posições exatas do enunciado, 6 normais exteriores
  unitárias, triangulação 12 triângulos com contrato de quad
  `(a,b,c)+(a,c,d)` e winding consistente, vértices editáveis (8) vs
  duplicados no buffer (24), ear clipping num polígono CÔNCAVO sem
  bowties (soma das áreas dos triângulos = área do polígono), normais e
  buffers atualizados após mover vértice/extrudir/desfazer, ciclo de
  seleção (pick V/A/F, multisseleção, prune), gizmo ponta-a-ponta com
  histórico (mover por eixo só mexe esse eixo; escala axial em torno do
  pivô; sinal do anel: o ponto sob o dedo segue o dedo) e desfazer
  byte-exato.
- **JVM Kotlin** (CI): 25 testes de câmara, gestos (incl. modo gizmo:
  alça→gizmo, falha→órbita, 2.º dedo cancela, tap em alça silencioso) e
  FPS.
- **Suíte de estabilidade** (`test_selection.cpp`, correção de
  estabilidade): seleção vazia → zero buffers de overlay e `hasValidSelection`
  falso; ids mortos não contados, nunca desenhados e removidos pela poda
  defensiva; seleção válida após extrudir/subdividir/loop cut/apagar e
  através de tempestades de undo/refazer (restauro da seleção pré/pós
  pelas entradas do histórico); rejeição de escala não finita (toque NaN
  /Inf) com a malha intacta; fator acumulado preso a [0.01, 100] (arrasto
  longo de encolhimento nunca achata a malha até zero); escala axial ×2
  seguida de ×0.5 restaura a geometria exata; extrusão com tampa movida:
  as 10 faces com normal para FORA (winding exterior seguro para
  culling) e normais finitas.
- **Suíte da reorganização da interface** (`test_ui_modes.cpp`): gizmo
  do MODO OBJETO transformando o objeto inteiro (mover/rodar/escalar com
  topologia invariante, invariantes de half-edge e undo/refazer byte-exato);
  transições Objeto↔Edição preservando a identidade do objeto ativo e a
  seleção de componentes (escondida, nunca perdida); `duplicateActiveObject`
  (clone byte-exato, deslocamento visível, ativação da cópia, undo remove
  e restaura o ativo, redo re-ativa); `transformDirect` (passos com
  histórico exato, rejeição de NaN/Inf/eixo inválido, clamp [0.01, 100],
  alvo = seleção válida em Edição vs objeto inteiro em Objeto); regra
  PARTILHADA do pivô (`gizmopivot::pivotFor`: objeto inteiro no Modo
  Objeto, centróide da seleção em Edição, sem pivô sem objeto — nunca
  há gizmo fantasma); o regressão do orçamento do histórico (operar
  APÓS desfazer já não falha com ErrBudget); e a sessão de ESCALA AXIAL
  do gizmo (contrato dos ids 21..23: `Tool::Scale` + kind 1 + eixo 0..2
  muda APENAS o eixo agarrado, em torno do centróide, topologia
  intacta, undo/refazer byte-exato, sessão rejeitada não fica presa).
- **Layouts/ciclo de vida (limitação declarada):** medições visuais
  (sobreposição de botões, corte em ecrãs pequenos, limite de 35% em
  paisagem) e comportamento de rotação/CONTEXT GL não têm testes
  automatizados neste projeto (sem Robolectric/dispositivo na CI) — a
  verificação continua a exigir dispositivo físico; a lógica pura
  (tracker, câmara, mapeamentos, seleção, escalas) está coberta por
  testes host/JVM.
- **Dispositivo físico** (responsabilidade do proprietário): sequência-mestre
  no app, gestos, rotação, FPS real no contador. O objetivo de 60 FPS é
  avaliado no dispositivo, nunca prometido por padrão.

## 8. Fases futuras (não implementadas aqui)

Fase 3: tinygltf (glTF 2.0/GLB) com versão fixada e licença verificada,
importador OBJ, exportação GLB/glTF, stb_image para texturas, xatlas para UV.
Nenhuma destas dependências ou funcionalidades foi integrada na Fase 2 —
`core/io/` está vazio de propósito.
