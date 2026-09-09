import { create } from "zustand";

/**
 * Client-side UI state ONLY.
 *
 * What belongs here: which panel is open, which symbol is selected, sidebar
 * collapsed or not. What must never appear here: positions, cash, P&L, orders,
 * or anything else the engine owns.
 *
 * The moment the client keeps its own copy of trading state, there are two
 * answers to "what do we hold?" and no way to tell which is right. Engine state
 * belongs in React Query, where it is a cache with a known origin and a
 * visible staleness -- not in a store that looks authoritative.
 */
interface UiState {
  sidebarCollapsed: boolean;
  selectedSymbol: string | null;
  toggleSidebar: () => void;
  selectSymbol: (symbol: string | null) => void;
}

export const useUiStore = create<UiState>((set) => ({
  sidebarCollapsed: false,
  selectedSymbol: null,
  toggleSidebar: () => set((s) => ({ sidebarCollapsed: !s.sidebarCollapsed })),
  selectSymbol: (selectedSymbol) => set({ selectedSymbol }),
}));
