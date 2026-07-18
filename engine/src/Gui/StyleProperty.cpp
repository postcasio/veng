#include <Veng/Gui/StyleProperty.h>

namespace Veng::Gui
{
    const char* ToString(StyleProperty property)
    {
        switch (property)
        {
        case StyleProperty::FlexDirection:
            return "flex-direction";
        case StyleProperty::JustifyContent:
            return "justify-content";
        case StyleProperty::AlignItems:
            return "align-items";
        case StyleProperty::AlignSelf:
            return "align-self";
        case StyleProperty::FlexWrap:
            return "flex-wrap";
        case StyleProperty::FlexGrow:
            return "flex-grow";
        case StyleProperty::FlexShrink:
            return "flex-shrink";
        case StyleProperty::FlexBasis:
            return "flex-basis";
        case StyleProperty::Width:
            return "width";
        case StyleProperty::Height:
            return "height";
        case StyleProperty::MinWidth:
            return "min-width";
        case StyleProperty::MinHeight:
            return "min-height";
        case StyleProperty::MaxWidth:
            return "max-width";
        case StyleProperty::MaxHeight:
            return "max-height";
        case StyleProperty::Margin:
            return "margin";
        case StyleProperty::Padding:
            return "padding";
        case StyleProperty::Position:
            return "position";
        case StyleProperty::Inset:
            return "inset";
        case StyleProperty::Background:
            return "background";
        case StyleProperty::CornerRadius:
            return "corner-radius";
        case StyleProperty::BorderWidth:
            return "border-width";
        case StyleProperty::BorderColor:
            return "border-color";
        case StyleProperty::TextColor:
            return "color";
        case StyleProperty::TextSize:
            return "font-size";
        case StyleProperty::TextFont:
            return "font";
        case StyleProperty::Opacity:
            return "opacity";
        case StyleProperty::Overflow:
            return "overflow";
        case StyleProperty::OverflowX:
            return "overflow-x";
        case StyleProperty::OverflowY:
            return "overflow-y";
        case StyleProperty::ScrollbarLayout:
            return "scrollbar";
        case StyleProperty::InsetLeft:
            return "inset-left";
        case StyleProperty::InsetTop:
            return "inset-top";
        case StyleProperty::InsetRight:
            return "inset-right";
        case StyleProperty::InsetBottom:
            return "inset-bottom";
        case StyleProperty::PointerEvents:
            return "pointer-events";
        case StyleProperty::Animation:
            return "animation";
        case StyleProperty::Origin:
            return "origin";
        case StyleProperty::BackgroundGradient:
            return "background-gradient";
        case StyleProperty::Rotation:
            return "rotation";
        case StyleProperty::TextAlign:
            return "text-align";
        }
        return "unknown";
    }

    optional<StyleProperty> ParseStyleProperty(std::string_view name)
    {
        if (name == "flex-direction")
        {
            return StyleProperty::FlexDirection;
        }
        if (name == "justify-content")
        {
            return StyleProperty::JustifyContent;
        }
        if (name == "align-items")
        {
            return StyleProperty::AlignItems;
        }
        if (name == "align-self")
        {
            return StyleProperty::AlignSelf;
        }
        if (name == "flex-wrap")
        {
            return StyleProperty::FlexWrap;
        }
        if (name == "flex-grow")
        {
            return StyleProperty::FlexGrow;
        }
        if (name == "flex-shrink")
        {
            return StyleProperty::FlexShrink;
        }
        if (name == "flex-basis")
        {
            return StyleProperty::FlexBasis;
        }
        if (name == "width")
        {
            return StyleProperty::Width;
        }
        if (name == "height")
        {
            return StyleProperty::Height;
        }
        if (name == "min-width")
        {
            return StyleProperty::MinWidth;
        }
        if (name == "min-height")
        {
            return StyleProperty::MinHeight;
        }
        if (name == "max-width")
        {
            return StyleProperty::MaxWidth;
        }
        if (name == "max-height")
        {
            return StyleProperty::MaxHeight;
        }
        if (name == "margin")
        {
            return StyleProperty::Margin;
        }
        if (name == "padding")
        {
            return StyleProperty::Padding;
        }
        if (name == "position")
        {
            return StyleProperty::Position;
        }
        if (name == "inset")
        {
            return StyleProperty::Inset;
        }
        if (name == "background")
        {
            return StyleProperty::Background;
        }
        if (name == "corner-radius")
        {
            return StyleProperty::CornerRadius;
        }
        if (name == "border-width")
        {
            return StyleProperty::BorderWidth;
        }
        if (name == "border-color")
        {
            return StyleProperty::BorderColor;
        }
        if (name == "color")
        {
            return StyleProperty::TextColor;
        }
        if (name == "font-size")
        {
            return StyleProperty::TextSize;
        }
        if (name == "font")
        {
            return StyleProperty::TextFont;
        }
        if (name == "opacity")
        {
            return StyleProperty::Opacity;
        }
        if (name == "overflow")
        {
            return StyleProperty::Overflow;
        }
        if (name == "overflow-x")
        {
            return StyleProperty::OverflowX;
        }
        if (name == "overflow-y")
        {
            return StyleProperty::OverflowY;
        }
        if (name == "scrollbar")
        {
            return StyleProperty::ScrollbarLayout;
        }
        if (name == "inset-left")
        {
            return StyleProperty::InsetLeft;
        }
        if (name == "inset-top")
        {
            return StyleProperty::InsetTop;
        }
        if (name == "inset-right")
        {
            return StyleProperty::InsetRight;
        }
        if (name == "inset-bottom")
        {
            return StyleProperty::InsetBottom;
        }
        if (name == "pointer-events")
        {
            return StyleProperty::PointerEvents;
        }
        if (name == "animation")
        {
            return StyleProperty::Animation;
        }
        if (name == "origin")
        {
            return StyleProperty::Origin;
        }
        if (name == "background-gradient")
        {
            return StyleProperty::BackgroundGradient;
        }
        if (name == "rotation")
        {
            return StyleProperty::Rotation;
        }
        if (name == "text-align")
        {
            return StyleProperty::TextAlign;
        }
        return std::nullopt;
    }
}
