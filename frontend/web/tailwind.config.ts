import type { Config } from "tailwindcss";

/**
 * Dark institutional palette.
 *
 * Deliberately low-chroma: on a terminal that is watched for hours, colour is
 * reserved for meaning. Green and red are for P&L direction and nothing else,
 * amber is degraded, and neutral greys carry the rest of the interface.
 */
const config: Config = {
  darkMode: "class",
  content: ["./app/**/*.{ts,tsx}", "./components/**/*.{ts,tsx}"],
  theme: {
    extend: {
      colors: {
        surface: {
          DEFAULT: "#0b0d10",
          raised: "#12161b",
          overlay: "#1a1f26",
          border: "#232a33",
        },
        content: {
          DEFAULT: "#e6e9ed",
          muted: "#8b95a3",
          faint: "#5a6472",
        },
        // Semantic only. A number is green because it is a gain, never for
        // decoration.
        gain: "#26a96c",
        loss: "#e5484d",
        warn: "#f0a202",
        info: "#3b82f6",
      },
      fontFamily: {
        // Tabular figures matter: columns of prices that jitter as digits
        // change are hard to read at a glance.
        mono: ["ui-monospace", "SFMono-Regular", "Menlo", "monospace"],
      },
    },
  },
  plugins: [],
};

export default config;
