/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  // Inlined at build time. A deployment sets this in Vercel's project
  // environment; the localhost default exists only so `npm run dev` works
  // with no configuration.
  env: {
    NEXT_PUBLIC_API_BASE: process.env.NEXT_PUBLIC_API_BASE ?? "http://localhost:8000",
  },
  // Trimmed output for container deploys. Vercel ignores this and uses its own
  // build pipeline.
  output: process.env.NEXT_OUTPUT_STANDALONE === "1" ? "standalone" : undefined,
  poweredByHeader: false,
};

export default nextConfig;
