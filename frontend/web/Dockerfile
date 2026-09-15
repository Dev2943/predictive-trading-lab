# Frontend image, for `docker compose` parity with production.
#
# Vercel does NOT use this file — it builds from source with its own pipeline.
# This exists so a local compose stack runs the same production build a reviewer
# would see deployed, rather than a dev server that behaves differently.
#
# Build context is frontend/web.

FROM node:22-alpine AS deps
WORKDIR /app
COPY package.json package-lock.json* ./
# `npm ci` when a lockfile is present: a reproducible install is the point of
# having one.
RUN npm ci --omit=dev 2>/dev/null || npm install --omit=dev

FROM node:22-alpine AS build
WORKDIR /app
COPY package.json package-lock.json* ./
RUN npm ci 2>/dev/null || npm install
COPY . .
# Baked at build time: NEXT_PUBLIC_* values are inlined into the bundle, so the
# API URL must be known here and cannot be changed by restarting the container.
ARG NEXT_PUBLIC_API_BASE=http://localhost:8000
ENV NEXT_PUBLIC_API_BASE=$NEXT_PUBLIC_API_BASE
RUN npm run build

FROM node:22-alpine AS runtime
WORKDIR /app
RUN addgroup -g 10001 ptl && adduser -u 10001 -G ptl -D ptl

COPY --from=build /app/.next ./.next
COPY --from=build /app/package.json ./package.json
COPY --from=deps /app/node_modules ./node_modules

USER ptl
ENV NODE_ENV=production PORT=3000
EXPOSE 3000

HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
    CMD wget -qO- http://127.0.0.1:3000/ >/dev/null || exit 1

CMD ["npm", "run", "start"]
