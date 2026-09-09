/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  // The API base is configuration, not a constant. When the engine moves behind
  // a remote gateway, this changes and no component does.
  env: {
    NEXT_PUBLIC_API_BASE: process.env.NEXT_PUBLIC_API_BASE ?? "http://localhost:8000",
  },
};

export default nextConfig;
