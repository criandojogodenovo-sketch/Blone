# Formato de Projeto Blone — "BLONEPRJ" (versão atual: 3)

> **Versão 3 (F4-LIMITS):** acrescenta uma **flag `editable` por objeto**.
> Objetos em **modo leitura** (imports acima do orçamento de edição do
> perfil, dentro do de visualização) guardam a geometria como **malha de
> renderização indexada** (posições + cores verbatim + índices u32) em vez
> de estruturas half-edge. O leitor aceita **v1, v2 e v3**; v1/v2 abrem com
> `editable = true` (comportamento antigo intacto). O escritor grava sempre
> a versão corrente (3). Aplicações antigas rejeitam v3 com o erro `-2` e
> uma mensagem clara — nunca interpretam às cegas.
>
> **Versão 2 (Fase 3):** cor por vértice (albedo RGB) para a ferramenta
> Pintar — mantida tal como estava.

Este documento define o formato binário próprio com que o Blone guarda e
abre projetos. Implementação: `app/src/main/cpp/core/project/Project.{h,cpp}`.

## 1. Garantias

- **Versionado**: o cabeçalho contém `formatVersion`. O leitor aceita as
  versões que conhece (1 e 2); versões mais recentes produzem o erro `-2`
  com mensagem clara — nunca são interpretadas às cegas.
- **Integridade**: CRC32 (IEEE, polinómio 0xEDB88320) sobre o payload.
  Ficheiros corrompidos são rejeitados com o erro `-4`.
- **Atomicidade na abertura**: o ficheiro é lido e validado num objeto `Scene`
  temporário (incluindo a validação completa da topologia half-edge de cada
  objeto). A cena atual do utilizador só é substituída em caso de sucesso —
  um ficheiro inválido **nunca destrói** o projeto aberto.
- **Sem caminhos externos**: o formato não referencia ficheiros do telemóvel;
  a gravação/abertura usa o Storage Access Framework (sem permissões de
  armazenamento abrangentes) e o Blone continua 100% offline.

## 2. Layout (little-endian)

```
Deslocamento  Tamanho  Campo
0             8        Magic "BLONEPRJ"
8             4        formatVersion (u32; atualmente 2 — v1 continua a abrir)
12            8        payloadLength (u64)
20            4        CRC32 (u32) do payload
24            ...      payload
```

## 3. Payload

```
i32  activeObject        (-1 quando nenhum)
u8   editMode            (0/1)
u8   selectionMode       (0 vértice, 1 aresta, 2 face)
u8   multiSelect         (0/1)
u8   reservado           (0)
u32  objectCount
repetido objectCount vezes:
    u32  nameLength (≤ 256)
    u8[nameLength]  nome em UTF-8
    f32 × 3         posição do objeto (translação; a Fase 2 não guarda
                    rotação/escala de objeto — podem ser adicionadas numa
                    versão futura do formato)
    v3 APENAS: u8   editable (1 = editável, 0 = modo leitura)
    editable == 1:  mesh (ver §4) + seleção (ver §5)
    editable == 0:  malha de leitura (ver §4-A) + seleção (sempre vazia)
```

## 4. Malha

Só os elementos **vivos** são gravados; os pools, mapas id→slot e o estado
derivado são reconstruídos na abertura. Os `id` são persistentes e monótonos
(0 é nulo), e as referências (twin/next/prev/face/edge/origin) são gravadas
como `id`, não como posições — a topologia sobrevive intacta.

```
u32 vertexCount
    por vértice:  u32 id; f32 x, y, z
                  v2 APENAS: f32 r, g, b   (cor albedo em [0,1]; na abertura,
                  valores não finitos são rejeitados com o erro -8 e valores
                  fora do intervalo são restringidos a [0,1]; ficheiros v1
                  recebem a cor predefinida kDefaultVertexColor)
u32 halfEdgeCount
    por half-edge: u32 id, origin, twin, next, prev, face (0 = fronteira), edge
u32 edgeCount
    por aresta:    u32 id, he
u32 faceCount
    por face:      u32 id, he
u32 × 4            reservado (o leitor recalcula os contadores de id como
                   maxId + 1 — defensivo contra valores corrompidos)
```

`vertex.outgoing` é **estado derivado** e não é serializado: na abertura é
recomputado como "o primeiro half-edge vivo com origem no vértice".

## 4-A. Malha de leitura (v3, `editable == 0`)

```
u32 vertexCount
    por vértice:  f32 x, y, z; f32 r, g, b   (posições/cores VERBATIM —
                  não finitos → erro -8; cores fora do intervalo → [0,1])
u32 indexCount (múltiplo de 3; senão erro -5)
    por índice:   u32 (cada valor < vertexCount, senão erro -8)
```

A geometria reconstrói um `RenderMesh` imutável partilhado (~30 B por
triângulo). A validação de topologia half-edge não se aplica; as contagens
respeitam os tetos de `MeshLimits`.

## 5. Seleção

```
u32 nV + nV × u32 id de vértices selecionados
u32 nE + nE × u32 id de arestas selecionadas
u32 nF + nF × u32 id de faces selecionadas
```

Na abertura, cada id é validado contra a malha; referência inválida → erro `-8`.

## 6. Erros de abertura (códigos JNI/Kotlin)

| Código | Significado |
|---|---|
| 0 | OK |
| -1 | Magic incorreto (não é um projeto Blone) |
| -2 | Versão do formato mais recente do que esta aplicação |
| -3 | Tamanho/declaração incoerente (ficheiro truncado ou com lixo final) |
| -4 | CRC não corresponde (ficheiro corrompido) |
| -5 | Contagens excedem os limites de geometria |
| -6 | Topologia inválida (invariantes half-edge não passam) |
| -7 | Payload truncado a meio de um campo |
| -8 | Referência/índice inválido (id inexistente ou morto) |

## 7. Evolução do formato

Regras para versões futuras (≥ 2):

1. Nunca alterar o significado dos campos existentes da mesma versão;
2. Acrescentar campos apenas no fim de cada secção, com contagens próprias;
3. Um leitor deve rejeitar (erro -2) versões que não conhece — conversões
   explícitas são implementadas caso a caso, nunca "adivinhadas";
4. Testes de round-trip (guardar → abrir → comparar byte a byte a geometria
   e a topologia) continuam obrigatórios a cada alteração.

## 8. Histórico das versões

- **v1 (Fase 2)** — geometria, topologia, seleção. Sem cores.
- **v2 (Fase 3)** — cor por vértice (3 × f32 por vértice, albedo RGB em
  [0,1]) para a ferramenta Pintar. Compatível: o leitor atual abre v1 (cores
  predefinidas) e v2; o leitor antigo rejeita v2 com erro claro.

Testes que fixam este contrato: `tests/test_brush.cpp`
(`testPersistenceV2RoundTrip`, `testV1FileCompatibility`).
