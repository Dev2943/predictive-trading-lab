# Frontend

Everything the web interface needs, in one directory. The engine at the
repository root is untouched and builds exactly as it did before.

```
frontend/
    backend/        FastAPI gateway + pybind11 bindings
    web/            Next.js interface
    docs/           Frontend architecture and phase notes
```

## The chain

```
React  →  FastAPI  →  pybind11  →  C++ engine
```

and later, with no change above the gateway:

```
React  →  FastAPI  →  HTTP  →  remote engine
```

That works because the binding boundary is **JSON, not objects**. Both
transports return the same parsed data, because both read the same serializers
inside the engine.

## Build

The engine builds with no Python, Node or frontend dependency of any kind:

```bash
cmake --preset linux-gcc-release && cmake --build build/linux-gcc-release
```

The bindings are a **standalone CMake project**. Nothing at the repository root
was changed to accommodate them:

```bash
pip install -r frontend/backend/requirements.txt

cmake -S frontend/backend/bindings -B build/bindings -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -Dpybind11_DIR=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())")
cmake --build build/bindings

PYTHONPATH=build/bindings/lib python3 -m pytest frontend/backend
```

Gateway and web:

```bash
cd frontend/backend && PYTHONPATH=../../build/bindings/lib uvicorn app.main:app --reload
cd frontend/web && npm install && npm run dev
```

## Rules this layer keeps

- **The engine is the single source of truth.** The frontend visualises engine
  state and computes none of it.
- **`Engine`, `PaperSession` and `LiveSession` are not exposed to Python.** A
  test asserts they are absent from the module.
- **No trading state in the client store.** Engine-derived data lives in React
  Query, where it is a cache with a visible origin and staleness — not in a
  store that would look authoritative.
