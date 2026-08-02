const COMPONENTS = Object.freeze({
  "node_runtime.private": {
    executable: { windows: "node.exe", linux: "node" },
    entrypoint: "node",
    licenseName: "node-LICENSE",
    capability: "private_node_runtime",
    smokeProtocol: "node_version",
  },
  "stockfish.engine": {
    executable: { windows: "stockfish.exe", linux: "stockfish" },
    entrypoint: "stockfish",
    licenseName: "stockfish-Copying.txt",
    capability: "uci_engine",
    discoveryKind: "uci_engine",
    smokeProtocol: "uci_startup",
  },
});

export function managedComponentDefinition(componentId) {
  const definition = COMPONENTS[componentId];
  if (!definition) {
    throw new Error(`unsupported managed component ${componentId}`);
  }
  return definition;
}

export function managedComponentManifest({
  componentId,
  version,
  target,
  executableRelative,
  payloadFiles,
}) {
  const definition = managedComponentDefinition(componentId);
  return {
    schema: "arena_install_component_v1",
    schema_version: 1,
    component_id: componentId,
    component_version: version,
    target_triple: target,
    entrypoints: { [definition.entrypoint]: executableRelative },
    ...(definition.discoveryKind
      ? {
          discovery: [{
            kind: definition.discoveryKind,
            locator: {
              source: "entrypoint",
              entrypoint: definition.entrypoint,
            },
          }],
        }
      : {}),
    capabilities: [definition.capability],
    startup: {},
    data_paths: [],
    files: payloadFiles,
  };
}
