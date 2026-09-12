"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";

/**
 * Navigation.
 *
 * Added in F5 because there is now a second page to navigate to. Building it in
 * F4, when the application had one route, would have been a shell around
 * nothing.
 */
const LINKS = [
  { href: "/", label: "Dashboard" },
  { href: "/trading", label: "Trading" },
  { href: "/optimization", label: "Optimization" },
  { href: "/market", label: "Market" },
  { href: "/analytics", label: "Analytics" },
  { href: "/research", label: "Research" },
  { href: "/risk", label: "Risk" },
];

export function Nav() {
  const pathname = usePathname();

  return (
    <nav aria-label="Main" className="mb-6 flex gap-1 border-b border-surface-border">
      {LINKS.map((link) => {
        const active = pathname === link.href;
        return (
          <Link
            key={link.href}
            href={link.href}
            aria-current={active ? "page" : undefined}
            className={`px-4 py-2 text-sm ${
              active
                ? "border-b-2 border-content text-content"
                : "text-content-faint hover:text-content-muted"
            }`}
          >
            {link.label}
          </Link>
        );
      })}
    </nav>
  );
}
