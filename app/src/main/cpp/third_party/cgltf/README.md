# cgltf (vendorizado)

- **Fonte oficial:** https://github.com/jkuhlmann/cgltf, tag `v1.14`
  (commit de release v1.14), ficheiros `cgltf.h` e `LICENSE` copiados SEM
  alterações.
- **Nota sobre `cgltf.c`:** a partir da v1.14 o cgltf passou a ser
  *single-header* — a implementação completa vive dentro de `cgltf.h`,
  guardada por `CGLTF_IMPLEMENTATION`. O `cgltf.c` deste diretório é a
  instanciação canónica de uma única unidade de tradução (padrão stb_*):
  define `CGLTF_IMPLEMENTATION` e inclui o header oficial. Nenhum código do
  parser foi modificado.
- **Licença:** MIT — ver `LICENSE` (copyright Johannes Kuhlmann).
- **Atualização:** substituir `cgltf.h` pela nova tag oficial e revalidar
  com a suite host (`tests/test_io.cpp` inclui o fixture `cube.glb`).
