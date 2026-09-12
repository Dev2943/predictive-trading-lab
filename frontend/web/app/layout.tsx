import type { Metadata } from "next";
import "./globals.css";
import { Nav } from "@/components/nav";
import { SessionBar } from "@/components/session-bar";
import { Providers } from "./providers";

export const metadata: Metadata = {
  title: "Predictive Trading Lab",
  description: "Interface for the Predictive Trading Lab engine.",
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en" className="dark">
      <body>
        <Providers>
          <div className="mx-auto max-w-6xl p-8 font-mono text-sm">
            <header className="mb-4">
              <h1 className="text-lg">Predictive Trading Lab</h1>
            </header>
            <Nav />
            <div className="mb-4">
              <SessionBar />
            </div>
            {children}
          </div>
        </Providers>
      </body>
    </html>
  );
}
