# Deployment

## 0. The constraint that shapes everything

**The backend is a single stateful process.** The paper session lives in memory
with one writer thread, and there are three process-level singletons: the engine
client, the session driver and the market data service.

The consequences are not negotiable:

- **One instance. One worker.** A second replica is a second session, and a
  client's requests would land on whichever one the load balancer picked.
  Scaling this service means redesigning session ownership, not raising a
  replica count.
- **Sessions do not survive a restart** unless a disk is attached at
  `PTL_RESULTS`. On a free tier that spins down, an idle deployment loses its
  session.

Both are stated in the UI and the README rather than discovered.

## 1. Local development

```bash
pip install -r frontend/backend/requirements.txt
cmake -S frontend/backend/bindings -B build/bindings -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -Dpybind11_DIR=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())")
cmake --build build/bindings

cd frontend/backend && PYTHONPATH=../../build/bindings/lib uvicorn app.main:app --reload
cd frontend/web && npm install && npm run dev
```

## 2. Docker

From the **repository root** — the backend build context is the root because
the bindings compile against the engine:

```bash
docker compose -f frontend/docker-compose.yml up --build
```

Backend on :8000, web on :3000. The web container waits for the backend's
readiness check.

## 3. Backend on Render

**Why Render**, chosen against the alternatives:

| Platform | Assessment |
|---|---|
| **Render** | **Chosen.** Native Docker with a long build timeout, which this image needs: it compiles the C++ engine. WebSockets work without configuration. A persistent disk can be attached at `/app/results`. Free tier is adequate for a demonstration if the spin-down is understood. |
| Railway | Comparable ergonomics and a faster build cache, but no durable free tier — a public demonstration would lapse when trial credit ran out. |
| Fly.io | Technically the best match: a single stateful machine with a volume is exactly this workload's shape, and `scale count 1` expresses the constraint directly. Rejected on operational surface — `fly.toml`, regions, volumes and machine lifecycle are more for a reviewer to learn than Render's single form. |

Steps:

1. New **Web Service** → your repository → **Docker**.
2. Dockerfile path `frontend/backend/Dockerfile`, **Docker context `.`** (the
   repository root — the default of the Dockerfile's directory will fail,
   because the engine source would not be in the context).
3. Instance type: at least the 512 MB tier. The C++ build needs headroom.
4. Environment:

   ```
   PTL_ENV=production
   PTL_ALLOWED_ORIGINS=https://<your-app>.vercel.app
   PTL_LOG_LEVEL=info
   PTL_RESULTS=/app/results
   PTL_MARKET_PROVIDER=replay
   ```

5. Optionally attach a disk at `/app/results` so sessions survive restarts.
6. Health check path: `/readyz`.

The first build is slow — it compiles the engine. Subsequent builds reuse the
layer unless engine sources change.

## 4. Frontend on Vercel

1. Import the repository; set **Root Directory** to `frontend/web`.
2. Environment variable:

   ```
   NEXT_PUBLIC_API_BASE=https://<your-service>.onrender.com
   ```

3. Deploy.

**`NEXT_PUBLIC_*` is inlined at build time.** Changing the API URL requires a
redeploy, not a restart. This is the single most common deployment confusion
with Next.js and is why the value is documented here rather than only in
`.env.example`.

## 5. CORS

The backend rejects, at startup, a production configuration whose
`PTL_ALLOWED_ORIGINS` contains `localhost` or `*`. Set it to the exact Vercel
origin, including the scheme and no trailing slash.

Preview deployments get their own origins. Add them explicitly, or accept that
previews cannot reach the API.

## 6. WebSocket

No separate configuration. The client derives the stream URL from
`NEXT_PUBLIC_API_BASE`, swapping `http` for `ws` — so an `https` API yields
`wss` automatically and avoids the mixed-content failure browsers report only
in the console.

Render terminates TLS and proxies WebSockets without extra settings.

## 7. Verify a deployment

```bash
curl https://<service>.onrender.com/healthz     # {"status":"ok",...}
curl https://<service>.onrender.com/readyz      # {"ready":true,...}
curl https://<service>.onrender.com/buildz      # engine version, commit, env
curl https://<service>.onrender.com/fingerprints
# config_hash must be 30b44e5972450aad
```

Then open the Vercel URL. The dashboard should show the engine version and
"reproduces the v1.0 reference build". If it shows **Backend unavailable**, the
service is asleep or the origin is wrong.

## 8. Troubleshooting

| Symptom | Cause |
|---|---|
| "Backend unavailable" on first load | Free tier waking from idle. It resolves in seconds; the page retries by itself. |
| CORS error in the browser console | `PTL_ALLOWED_ORIGINS` does not match the Vercel origin exactly. |
| API calls go to `localhost` in production | `NEXT_PUBLIC_API_BASE` was not set **at build time**. Redeploy. |
| Backend exits immediately at startup | Configuration validation failed. The log names every problem and its remedy. |
| Session disappears after a while | Free-tier spin-down with no attached disk. Expected; attach a disk to persist. |
| WebSocket fails on https | `NEXT_PUBLIC_API_BASE` uses `http`, producing `ws` on an `https` page. |

## 9. Checklists

**Deployment**

- [ ] `PTL_ENV=production`
- [ ] `PTL_ALLOWED_ORIGINS` set to the exact frontend origin
- [ ] `PTL_DEBUG` unset
- [ ] Health check path `/readyz`
- [ ] Disk attached if sessions must survive restarts
- [ ] `NEXT_PUBLIC_API_BASE` set in Vercel **before** building
- [ ] `/fingerprints` returns `30b44e5972450aad`

**Release**

- [ ] Backend, frontend and engine tests pass
- [ ] `next build` succeeds
- [ ] Docker images build and the backend reaches `/readyz`
- [ ] OpenAPI regenerated with no missing summaries
- [ ] No secrets committed — `.env` is git-ignored
- [ ] `DEPLOYMENT.md` matches the deployed configuration
