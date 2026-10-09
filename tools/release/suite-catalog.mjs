import { validateCompositionManifests } from "./product-composition.mjs";

function positiveOrComputed(value, computed, label) {
  const selected = value ?? computed;
  if (!Number.isSafeInteger(selected) || selected < 0) {
    throw new Error(`${label} must be a nonnegative safe integer`);
  }
  return selected;
}

function artifactType(candidate) {
  const declared = candidate.verified?.artifact.type??candidate.lockRow.artifact_type
    ?? candidate.providerResult?.artifact_type;
  if (declared) return declared;
  return candidate.lockRow.artifact_name.endsWith(".file-set.json")
    ? "file_set" : "archive";
}

function buildArtifact(candidate) {
  const type = artifactType(candidate);
  const artifact = {
    type,
    source: `payloads/${candidate.verified?.artifact.name??candidate.lockRow.artifact_name}`,
    sha256: candidate.verified?.artifact.sha256??candidate.lockRow.artifact_sha256,
    size_bytes: candidate.verified?.artifact.size_bytes??candidate.artifactSizeBytes,
  };
  if (type === "file_set") {
    artifact.file_count = candidate.verified?.artifact.file_count??candidate.lockRow.file_count
      ?? candidate.providerResult?.file_set_file_count;
    artifact.total_size_bytes = candidate.verified?.artifact.installed_bytes??candidate.lockRow.installed_bytes
      ?? candidate.providerResult?.file_set_total_size_bytes;
    if (!Number.isSafeInteger(artifact.file_count) || artifact.file_count < 1
        || !Number.isSafeInteger(artifact.total_size_bytes)
        || artifact.total_size_bytes < 1) {
      throw new Error(
        `${candidate.lockRow.component_id} file-set capacity is incomplete`,
      );
    }
  }
  return artifact;
}

export function buildSuiteCatalog({
  composition,
  target,
  candidates,
  featureComponentIds,
  channel = "development",
}) {
  if (!Array.isArray(candidates) || candidates.length !== composition.components.length) {
    throw new Error("suite catalog requires one candidate per composed component");
  }
  const resolved = validateCompositionManifests(
    composition,
    candidates.map((candidate) => ({
      component_id: candidate.lockRow.component_id,
      version: candidate.lockRow.component_version,
      repository: candidate.repository,
      manifest: candidate.manifest,
    })),
  );
  const byId = new Map(
    candidates.map((candidate) => [candidate.lockRow.component_id, candidate]),
  );
  if (!Array.isArray(featureComponentIds)
      || new Set(featureComponentIds).size !== featureComponentIds.length
      || featureComponentIds.some((id) => !byId.has(id))) {
    throw new Error("feature policy contains an unavailable or duplicate component");
  }
  const manager = composition.components.find((component) => component.role === "manager");
  if (!manager) throw new Error("product composition has no Manager role");

  const components = resolved.map((component) => {
    const candidate = byId.get(component.component_id);
    const installedBytes = candidate.manifest.files.reduce(
      (total, file) => total + file.size_bytes,
      candidate.manifestSizeBytes,
    );
    const activationRole = Array.isArray(candidate.manifest.activation)
      && candidate.manifest.activation.length > 0 ? "knowledge" : "component";
    return {
      id: component.component_id,
      version: component.version,
      dependencies: [...component.requires],
      installed_bytes: positiveOrComputed(
        candidate.lockRow.installed_bytes, installedBytes, `${component.component_id}.installed_bytes`,
      ),
      staging_bytes: positiveOrComputed(
        candidate.lockRow.staging_bytes, installedBytes, `${component.component_id}.staging_bytes`,
      ),
      backup_bytes: positiveOrComputed(
        candidate.lockRow.backup_bytes, installedBytes, `${component.component_id}.backup_bytes`,
      ),
      workspace_reserve_bytes: positiveOrComputed(
        candidate.lockRow.workspace_reserve_bytes, 0,
        `${component.component_id}.workspace_reserve_bytes`,
      ),
      default_enabled: component.default_enabled,
      activation_role: activationRole,
      artifacts: {
        [candidate.lockRow.target_triple.startsWith("any")
          ? "any" : target]: buildArtifact(candidate),
      },
    };
  });
  const knowledge = components
    .filter((component) => component.activation_role === "knowledge")
    .map((component) => component.id);
  const books = candidates
    .filter((candidate) => candidate.manifest.activation?.some(
      (activation) => activation.kind === "opening_book",
    ))
    .map((candidate) => candidate.lockRow.component_id);
  const tablebases = candidates
    .filter((candidate) => candidate.manifest.activation?.some(
      (activation) => activation.kind === "syzygy",
    ))
    .map((candidate) => candidate.lockRow.component_id);
  return {
    schema: "arena_install_catalog_v1",
    schema_version: 1,
    suite_version: composition.product_release,
    channel,
    presets: {
      manager_only: [manager.component_id],
      complete_arena: composition.components
        .filter((component) => component.default_enabled)
        .map((component) => component.component_id),
    },
    optional_component_groups: { knowledge, books, tablebases },
    feature_selection: {
      base_preset: "manager_only",
      components: [...featureComponentIds],
      default_selected: featureComponentIds.filter(
        (id) => composition.components.find(
          (component) => component.component_id === id,
        ).default_enabled,
      ),
    },
    components,
  };
}
