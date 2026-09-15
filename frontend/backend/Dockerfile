# Backend image: FastAPI gateway + compiled pybind11 bindings.
#
# BUILD CONTEXT IS THE REPOSITORY ROOT, not this directory. The bindings compile
# against the engine's headers and libraries, so the engine source must be
# visible to the build:
#
#   docker build -f frontend/backend/Dockerfile -t ptl-backend .
#
# The engine is COMPILED here and never modified. Nothing in this file writes to
# include/ or src/.
#
# Multi-stage because the toolchain is large: GCC, CMake, Ninja and Eigen are
# needed to produce one ~1.5 MB shared object, and shipping them would make the
# runtime image an order of magnitude bigger for no benefit.

# ---------------------------------------------------------------------------
# Stage 1 — compile the engine and the bindings
# ---------------------------------------------------------------------------
FROM python:3.12-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        libeigen3-dev \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir pybind11

WORKDIR /src
# Only what the binding build reads. Copying the whole tree would invalidate
# this layer on every frontend edit, and the C++ build is the slow one.
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY include ./include
COPY src ./src
COPY config ./config
COPY frontend/backend/bindings ./frontend/backend/bindings

RUN cmake -S frontend/backend/bindings -B /build/bindings -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -Dpybind11_DIR="$(python3 -c 'import pybind11; print(pybind11.get_cmake_dir())')" \
    && cmake --build /build/bindings

# ---------------------------------------------------------------------------
# Stage 2 — runtime
# ---------------------------------------------------------------------------
FROM python:3.12-slim AS runtime

# NON-ROOT. A process that can write its own code is one compromise away from
# persisting.
RUN useradd --create-home --uid 10001 ptl

WORKDIR /app

COPY frontend/backend/requirements.txt ./requirements.txt
RUN pip install --no-cache-dir -r requirements.txt

COPY --from=build /build/bindings/lib/ptl*.so /app/lib/
COPY frontend/backend/app ./app
# The engine reads its configuration at runtime for the fingerprint endpoints.
COPY config ./config

# Session artifacts. Declared as a volume so a platform can attach a disk;
# without one the directory is ephemeral and sessions do not survive a restart,
# which is documented rather than hidden.
RUN mkdir -p /app/results && chown -R ptl:ptl /app
VOLUME ["/app/results"]

USER ptl

ENV PYTHONPATH=/app/lib \
    PYTHONUNBUFFERED=1 \
    PTL_RESULTS=/app/results \
    PTL_CONFIG=config/base.toml \
    PTL_ENV=production \
    PTL_HOST=0.0.0.0 \
    PORT=8000

EXPOSE 8000

# Readiness, not liveness: the container is useful only once the bindings
# import, and a health check that passed before that would route traffic to a
# process that cannot answer.
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD python -c "import urllib.request,sys,json; \
r=json.load(urllib.request.urlopen('http://127.0.0.1:'+__import__('os').environ.get('PORT','8000')+'/readyz')); \
sys.exit(0 if r.get('ready') else 1)"

# ONE WORKER, DELIBERATELY. The gateway holds the paper session in memory in a
# single process with one writer thread. A second worker would be a second
# session, and a client's requests would land on whichever one the load
# balancer chose. Scaling this service means scaling sessions, which is a
# design change, not a flag.
CMD ["sh", "-c", "uvicorn app.main:app --host ${PTL_HOST:-0.0.0.0} --port ${PORT:-8000} --workers 1 --log-level ${PTL_LOG_LEVEL:-info}"]
