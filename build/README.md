This directory is reserved for local out-of-source builds.

Typical usage:

```bash
cmake ..
make
```

Files generated inside `build/` are intentionally ignored by Git.
Do not commit build cache, object files, or other intermediate outputs from this directory.
