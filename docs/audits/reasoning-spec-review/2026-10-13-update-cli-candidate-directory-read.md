# Restore the public v3 component-update entrypoint

The public package-only update CLI references readFileSync for its declared v3 candidate-directory map without importing it. An exact real eight-component diagnostic lock reproduced `readFileSync is not defined` before verification or extraction. Earlier operator packets used the exported common verifier/transactional assembly surfaces, preserving every cryptographic gate, but the product CLI itself remained broken.

This concerns Installer I-O1/I-O2, suite G-O2 and C08/C09/C11: a correct public release workflow must reach its existing full-provenance gates. Importing the existing reader is selected over maintaining operator wrappers or inventing a separate input authority. No signer policy, lock schema, candidate verification, extraction, scope or native build semantics change.

A credential-free optional real-lock CLI regression copies the current release source into an owned clean fixture repository, supplies a malformed declared map, and requires the actual JSON-reader rejection before verification while proving no output publication. The clean fixture preserves sourceIdentity's clean-tree gate even while implementation work is in progress. It is internal qualification; a real full candidate-map packaging run also remains required before this repair is accepted as working.
