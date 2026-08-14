import type { GlobalThemeOverrides } from "naive-ui";

/** Teal industrial palette aligned with existing CSS tokens (not default purple). */
export const naiveThemeOverrides: GlobalThemeOverrides = {
  common: {
    primaryColor: "#1f6b5c",
    primaryColorHover: "#2a8573",
    primaryColorPressed: "#0f3d34",
    primaryColorSuppl: "#246453",
    infoColor: "#1f6b5c",
    successColor: "#246453",
    warningColor: "#a36a1d",
    errorColor: "#b03a2e",
    textColorBase: "#15201c",
    textColor1: "#15201c",
    textColor2: "#3d4d47",
    textColor3: "#6a7a73",
    borderColor: "#c8d1cb",
    dividerColor: "#c8d1cb",
    hoverColor: "rgba(31, 107, 92, 0.08)",
    bodyColor: "#eef1ee",
    cardColor: "#f8faf8",
    modalColor: "#f8faf8",
    popoverColor: "#f8faf8",
    tableColor: "#f8faf8",
    inputColor: "#ffffff",
    actionColor: "#d7ebe4",
    borderRadius: "8px",
    fontFamily:
      '"IBM Plex Sans", "Noto Sans SC", "PingFang SC", sans-serif',
    fontFamilyMono: '"IBM Plex Mono", "SF Mono", ui-monospace, monospace'
  },
  Button: {
    borderRadiusMedium: "8px",
    heightMedium: "34px"
  },
  Card: {
    borderRadius: "10px",
    color: "#f8faf8"
  },
  DataTable: {
    thColor: "#f4f7f5",
    tdColor: "#f8faf8",
    borderColor: "#c8d1cb"
  },
  Menu: {
    itemTextColor: "#c5d0ca",
    itemTextColorHover: "#eef5f1",
    itemTextColorActive: "#e8fff6",
    itemTextColorActiveHover: "#e8fff6",
    itemIconColor: "#9aaca3",
    itemIconColorHover: "#eef5f1",
    itemIconColorActive: "#7fd0b5",
    itemIconColorActiveHover: "#7fd0b5",
    itemColorHover: "#222c28",
    itemColorActive: "#254a40",
    itemColorActiveHover: "#254a40",
    arrowColor: "#9aaca3"
  },
  Layout: {
    color: "#eef1ee",
    siderColor: "#141a18",
    headerColor: "rgba(248, 250, 248, 0.92)"
  },
  Tag: {
    borderRadius: "6px"
  }
};
