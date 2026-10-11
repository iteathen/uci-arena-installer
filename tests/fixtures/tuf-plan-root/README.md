# Public TUF test fixture

Synthetic Ed25519-signed chain for native plan-to-confirm root-permission tests. Only public metadata, signatures, and an empty catalog are retained. The test signing key was generated in memory and discarded without serialization. The production compiled trust root is unchanged; only the never-packaged CLI fixture target pins this public root hash. This grants no release or installed-registration authority.
