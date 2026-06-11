#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

namespace alryn::ui {

// Visual style shared by every widget. Tweak the global instance (theme()) once
// and the whole UI follows. Defaults are a clean, modern dark palette.
struct Theme {
    Vec4 panel{0.11f, 0.12f, 0.16f, 0.94f};   // card / panel background
    Vec4 panel_border{1.0f, 1.0f, 1.0f, 0.07f};
    Vec4 overlay{0.04f, 0.05f, 0.08f, 0.55f}; // dim behind a modal menu

    Vec4 text{0.93f, 0.95f, 0.98f, 1.0f};
    Vec4 text_muted{0.62f, 0.66f, 0.74f, 1.0f};

    Vec4 accent{0.28f, 0.60f, 0.96f, 1.0f};       // primary action / fills
    Vec4 accent_hover{0.40f, 0.71f, 1.0f, 1.0f};

    Vec4 button{0.19f, 0.21f, 0.27f, 1.0f};
    Vec4 button_hover{0.25f, 0.29f, 0.38f, 1.0f};
    Vec4 button_press{0.15f, 0.17f, 0.22f, 1.0f};

    Vec4 track{0.20f, 0.22f, 0.28f, 1.0f}; // slider/toggle track
    Vec4 knob{0.90f, 0.93f, 0.97f, 1.0f};

    f32 radius = 10.0f;
};

// The process-wide UI theme (mutable so games can restyle).
Theme& theme();

} // namespace alryn::ui
