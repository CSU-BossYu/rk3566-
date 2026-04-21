# RK3566 Project

This repository keeps source code, configuration, and deployable assets for the RK3566 project.

## Build

Use an out-of-source build from the `build/` directory:

```bash
mkdir -p build
cd build
cmake ..
make
```

## About `build/`

The `build/` directory is kept in the repository only as a placeholder for local builds.
Generated files inside `build/` are ignored by Git and are not meant to be committed.

If you need distributable outputs, use the tracked files under `deploy/` instead of committing local build cache or intermediate artifacts from `build/`.
