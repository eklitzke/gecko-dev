/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

var EXPORTED_SYMBOLS = ["ThemeVariableMap"];

const ThemeVariableMap = [
  ["--lwt-accent-color-inactive", {
    lwtProperty: "accentcolorInactive"
  }],
  ["--lwt-background-alignment", {
    isColor: false,
    lwtProperty: "backgroundsAlignment"
  }],
  ["--lwt-background-tiling", {
    isColor: false,
    lwtProperty: "backgroundsTiling"
  }],
  ["--tab-loading-fill", {
    lwtProperty: "tab_loading",
    optionalElementID: "tabbrowser-tabs"
  }],
  ["--lwt-tab-text", {
    lwtProperty: "tab_text"
  }],
  ["--tab-line-color", {
    lwtProperty: "tab_line",
    optionalElementID: "tabbrowser-tabs"
  }],
  ["--toolbar-bgcolor", {
    lwtProperty: "toolbarColor"
  }],
  ["--toolbar-color", {
    lwtProperty: "toolbar_text"
  }],
  ["--lwt-toolbar-field-border-color", {
    lwtProperty: "toolbar_field_border"
  }],
  ["--lwt-toolbar-field-focus", {
    lwtProperty: "toolbar_field_focus"
  }],
  ["--lwt-toolbar-field-focus-color", {
    lwtProperty: "toolbar_field_text_focus"
  }],
  ["--toolbar-field-focus-border-color", {
    lwtProperty: "toolbar_field_border_focus"
  }],
  ["--urlbar-separator-color", {
    lwtProperty: "toolbar_field_separator"
  }],
  ["--tabs-border-color", {
    lwtProperty: "toolbar_top_separator",
    optionalElementID: "navigator-toolbox"
  }],
  ["--lwt-toolbar-vertical-separator", {
    lwtProperty: "toolbar_vertical_separator"
  }],
  ["--toolbox-border-bottom-color", {
    lwtProperty: "toolbar_bottom_separator"
  }],
  ["--lwt-toolbarbutton-icon-fill", {
    lwtProperty: "icon_color"
  }],
  ["--lwt-toolbarbutton-icon-fill-attention", {
    lwtProperty: "icon_attention_color"
  }],
  ["--lwt-toolbarbutton-hover-background", {
    lwtProperty: "button_background_hover"
  }],
  ["--lwt-toolbarbutton-active-background", {
    lwtProperty: "button_background_active"
  }],
  ["--lwt-selected-tab-background-color", {
    lwtProperty: "tab_selected"
  }],
  ["--autocomplete-popup-background", {
    lwtProperty: "popup"
  }],
  ["--autocomplete-popup-color", {
    lwtProperty: "popup_text",
    processColor(rgbaChannels, element) {
      const secondaryVariable = "--autocomplete-popup-secondary-color";

      if (!rgbaChannels) {
        element.removeAttribute("lwt-popup-brighttext");
        element.style.removeProperty(secondaryVariable);
        return null;
      }

      let {r, g, b, a} = rgbaChannels;
      let luminance = 0.2125 * r + 0.7154 * g + 0.0721 * b;

      if (luminance <= 110) {
        element.removeAttribute("lwt-popup-brighttext");
      } else {
        element.setAttribute("lwt-popup-brighttext", "true");
      }

      element.style.setProperty(secondaryVariable, `rgba(${r}, ${g}, ${b}, 0.5)`);
      return `rgba(${r}, ${g}, ${b}, ${a})`;
    }
  }],
  ["--autocomplete-popup-border-color", {
    lwtProperty: "popup_border"
  }],
  ["--autocomplete-popup-highlight-background", {
    lwtProperty: "popup_highlight"
  }],
  ["--autocomplete-popup-highlight-color", {
    lwtProperty: "popup_highlight_text"
  }],
];
