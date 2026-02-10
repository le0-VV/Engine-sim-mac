#include "../include/ui_element.h"

#include "../include/engine_sim_application.h"
#include "../include/debug_trace.h"

#include <assert.h>
#include <chrono>
#include <cmath>
#include <typeinfo>
#include <unordered_map>

namespace {
struct WidgetTraceState {
    Bounds bounds;
    bool visible = true;
    bool culled = false;
    int z = -1;
    bool initialized = false;
};

std::unordered_map<const UiElement *, WidgetTraceState> g_widgetState;

bool boundsChanged(const Bounds &a, const Bounds &b) {
    return a.m0.x != b.m0.x
        || a.m0.y != b.m0.y
        || a.m1.x != b.m1.x
        || a.m1.y != b.m1.y;
}

bool pointsEqual(const Point &a, const Point &b) {
    constexpr float PositionEpsilon = 1.0e-4f;
    return std::fabs(a.x - b.x) <= PositionEpsilon
        && std::fabs(a.y - b.y) <= PositionEpsilon;
}

bool isOffscreen(const Bounds &b, int screenWidth, int screenHeight) {
    if (screenWidth <= 0 || screenHeight <= 0) return false;
    if (b.right() < 0 || b.left() > screenWidth) return true;
    if (b.bottom() < 0 || b.top() > screenHeight) return true;
    return false;
}
} /* namespace */

UiElement::UiElement() {
    m_app = nullptr;
    m_parent = nullptr;
    m_signalTarget = nullptr;
    m_checkMouse = false;
    m_disabled = false;
    m_index = -1;

    m_draggable = false;
    m_mouseOver = false;
    m_mouseHeld = false;
    m_visible = true;
}

UiElement::~UiElement() {
    /* void */
}

void UiElement::initialize(EngineSimApplication *app) {
    m_app = app;
}

void UiElement::destroy() {
    for (UiElement *child : m_children) {
        child->destroy();
        delete child;
    }

    m_children.clear();
}

void UiElement::update(float dt) {
    for (UiElement *child : m_children) {
        child->update(dt);
    }
}

void UiElement::render() {
    const int screenW = (m_app != nullptr) ? m_app->getScreenWidth() : 0;
    const int screenH = (m_app != nullptr) ? m_app->getScreenHeight() : 0;
    for (UiElement *child : m_children) {
        const Bounds renderBounds = child->unitsToPixels(child->getRenderBounds(child->m_bounds));
        WidgetTraceState &state = g_widgetState[child];
        if (!state.initialized || boundsChanged(state.bounds, renderBounds)) {
            DebugTrace::Log(
                "ui",
                "widget invalidation reason=BOUNDS_CHANGED id=%p name=%s bounds=(%.2f,%.2f,%.2f,%.2f)",
                child,
                child->getDebugName(),
                renderBounds.left(),
                renderBounds.bottom(),
                renderBounds.width(),
                renderBounds.height());
            state.bounds = renderBounds;
            state.initialized = true;
        }

        const bool visible = child->isVisible();
        const bool culledOffscreen = isOffscreen(renderBounds, screenW, screenH);
        const bool shouldDraw = visible && !culledOffscreen;
        if (!state.initialized
            || state.visible != visible
            || state.culled != culledOffscreen
            || state.z != child->m_index) {
            DebugTrace::Log(
                "ui",
                "widget visibility id=%p name=%s visible=%d culled=%d reason=%s z=%d layer=%d bounds=(%.2f,%.2f,%.2f,%.2f)",
                child,
                child->getDebugName(),
                visible ? 1 : 0,
                shouldDraw ? 0 : 1,
                (!visible) ? "HIDDEN" : (culledOffscreen ? "OFFSCREEN" : "VISIBLE"),
                child->m_index,
                0x11,
                renderBounds.left(),
                renderBounds.bottom(),
                renderBounds.width(),
                renderBounds.height());
            state.visible = visible;
            state.culled = culledOffscreen;
            state.z = child->m_index;
        }

        if (!shouldDraw) {
            if (visible && culledOffscreen) {
                DebugTrace::Log(
                    "ui",
                    "dead_hidden_widget_draw_attempt id=%p name=%s visible=%d culled=%d",
                    child,
                    child->getDebugName(),
                    visible ? 1 : 0,
                    culledOffscreen ? 1 : 0);
            }
            continue;
        }

        const auto t0 = std::chrono::steady_clock::now();
        DebugTrace::Log("ui", "widget draw begin id=%p name=%s z=%d", child, child->getDebugName(), child->m_index);
        child->render();
        const auto t1 = std::chrono::steady_clock::now();
        DebugTrace::Log(
            "ui",
            "widget draw end id=%p name=%s duration_us=%lld",
            child,
            child->getDebugName(),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
    }
}

const char *UiElement::getDebugName() const {
    return typeid(*this).name();
}

void UiElement::signal(UiElement *element, Event event) {
    /* void */
}

void UiElement::onMouseDown(const Point &mouseLocal) {
    m_mouseHeld = true;
}

void UiElement::onMouseUp(const Point &mouseLocal) {
    m_mouseHeld = false;
}

void UiElement::onMouseClick(const Point &mouseLocal) {
    signal(Event::Clicked);
}

void UiElement::onDrag(const Point &p0, const Point &mouse0, const Point &mouse) {
    if (m_draggable) {
        m_localPosition = p0 + (mouse - mouse0);
    }
}

void UiElement::onMouseOver(const Point &mouseLocal) {
    m_mouseOver = true;
}

void UiElement::onMouseLeave() {
    m_mouseOver = false;
}

void UiElement::onMouseScroll(int mouseScroll) {
    /* void */
}

UiElement *UiElement::mouseOver(const Point &mouseLocal) {
    if (m_disabled) return nullptr;

    const int n = (int)getChildCount();
    for (int i = n - 1; i >= 0; --i) {
        UiElement *child = m_children[i];
        UiElement *clickedElement = child->mouseOver(mouseLocal - child->m_localPosition);
        if (clickedElement != nullptr) {
            return clickedElement;
        }
    }

    return (m_checkMouse && m_mouseBounds.overlaps(mouseLocal))
        ? this
        : nullptr;
}

Point UiElement::getWorldPosition() const {
    return (m_parent != nullptr)
        ? m_parent->getWorldPosition() + m_localPosition
        : m_localPosition;
}

void UiElement::setLocalPosition(const Point &p, const Point &ref) {
    const Point current = m_bounds.getPosition(ref) + m_localPosition;
    const Point nextPosition = m_localPosition + (p - current);
    if (pointsEqual(nextPosition, m_localPosition)) return;
    m_localPosition = nextPosition;
    DebugTrace::Log(
        "ui",
        "widget invalidation reason=LOCAL_POSITION id=%p name=%s local_pos=(%.2f,%.2f)",
        this,
        getDebugName(),
        m_localPosition.x,
        m_localPosition.y);
}

void UiElement::setLocalPosition(const Point &p) {
    if (pointsEqual(p, m_localPosition)) return;
    m_localPosition = p;
    DebugTrace::Log(
        "ui",
        "widget invalidation reason=LOCAL_POSITION id=%p name=%s local_pos=(%.2f,%.2f)",
        this,
        getDebugName(),
        m_localPosition.x,
        m_localPosition.y);
}

void UiElement::setVisible(bool visible) {
    if (m_visible == visible) return;
    m_visible = visible;
    DebugTrace::Log(
        "ui",
        "widget invalidation reason=VISIBILITY id=%p name=%s visible=%d",
        this,
        getDebugName(),
        m_visible ? 1 : 0);
}

void UiElement::bringToFront(UiElement *element) {
    assert(element->m_parent == this);
    if (m_children.empty()) return;
    if (m_children.back() == element) return;

    m_children.erase(m_children.begin() + element->m_index);
    m_children.push_back(element);

    int i = 0;
    for (UiElement *element : m_children) {
        element->m_index = i++;
    }

    DebugTrace::Log(
        "ui",
        "widget invalidation reason=Z_ORDER id=%p name=%s new_z=%d",
        element,
        element->getDebugName(),
        element->m_index);
}

void UiElement::activate() {
    bool zOrderChanged = false;
    if (m_parent != nullptr) {
        const UiElement *currentFront = m_parent->getChild(m_parent->getChildCount() - 1);
        if (currentFront != this) {
            m_parent->bringToFront(this);
            zOrderChanged = true;
        }
        m_parent->activate();
    }

    if (!zOrderChanged) return;
    DebugTrace::Log(
        "ui",
        "widget invalidation reason=ACTIVATE id=%p name=%s",
        this,
        getDebugName());
}

void UiElement::signal(Event event) {
    if (m_signalTarget == nullptr) return;

    m_signalTarget->signal(this, event);
}

float UiElement::pixelsToUnits(float length) const {
    return length;// m_app->pixelsToUnits(length);
}

Point UiElement::pixelsToUnits(const Point &p) const {
    return { pixelsToUnits(p.x), pixelsToUnits(p.y) };
}

float UiElement::unitsToPixels(float x) const {
    return x; // m_app->unitsToPixels(x);
}

Point UiElement::unitsToPixels(const Point &p) const {
    return { unitsToPixels(p.x), unitsToPixels(p.y) };
}

Point UiElement::getRenderPoint(const Point &p) const {
    const Point offset(
            -(float)m_app->getScreenWidth() / 2,
            -(float)m_app->getScreenHeight() / 2);
    const Point posPixels = localToWorld(p) + offset;

    return pixelsToUnits(posPixels);
}

Bounds UiElement::getRenderBounds(const Bounds &b) const {
    return { getRenderPoint(b.m0), getRenderPoint(b.m1) };
}

Bounds UiElement::unitsToPixels(const Bounds &b) const {
    return { unitsToPixels(b.m0), unitsToPixels(b.m1) };
}

void UiElement::resetShader() {
    m_app->getShaders()->ResetBaseColor();
    m_app->getShaders()->SetObjectTransform(ysMath::LoadIdentity());
}

void UiElement::drawModel(
    dbasic::ModelAsset *model,
    const ysVector &color,
    const Point &p,
    const Point &s)
{
    resetShader();

    const Point p_render = getRenderPoint(p);
    const Point s_render = pixelsToUnits(s);

    m_app->getShaders()->SetObjectTransform(
        ysMath::MatMult(
            ysMath::TranslationTransform(ysMath::LoadVector(p_render.x, p_render.y, 0.0)),
            ysMath::ScaleTransform(ysMath::LoadVector(s_render.x, s_render.y, 0.0))
        )
    );

    m_app->getShaders()->SetBaseColor(color);
    m_app->getEngine()->DrawModel(m_app->getShaders()->GetUiFlags(), model, 0x11);
}

void UiElement::drawFrame(
        const Bounds &bounds,
        float thickness,
        const ysVector &frameColor,
        const ysVector &fillColor,
        bool fill)
{
    GeometryGenerator *generator = m_app->getGeometryGenerator();

    const Bounds worldBounds = getRenderBounds(bounds);
    const Point position = worldBounds.getPosition(Bounds::center);

    GeometryGenerator::FrameParameters params;
    params.frameWidth = worldBounds.width();
    params.frameHeight = worldBounds.height();
    params.lineWidth = pixelsToUnits(thickness);
    params.x = position.x;
    params.y = position.y;

    GeometryGenerator::Line2dParameters lineParams;
    lineParams.lineWidth = worldBounds.height();
    lineParams.y0 = lineParams.y1 = worldBounds.getPosition(Bounds::center).y;
    lineParams.x0 = worldBounds.left();
    lineParams.x1 = worldBounds.right();

    GeometryGenerator::GeometryIndices frame, body;

    resetShader();
    if (fill) {
        generator->startShape();
        generator->generateLine2d(lineParams);
        generator->endShape(&body);

        m_app->getShaders()->SetBaseColor(fillColor);
        m_app->drawGenerated(body, 0x11, m_app->getShaders()->GetUiFlags());
    }

    generator->startShape();
    generator->generateFrame(params);
    generator->endShape(&frame);

    m_app->getShaders()->SetBaseColor(frameColor);
    m_app->drawGenerated(frame, 0x11, m_app->getShaders()->GetUiFlags());
}

void UiElement::drawBox(const Bounds &bounds, const ysVector &fillColor) {
    GeometryGenerator *generator = m_app->getGeometryGenerator();
    const Bounds worldBounds = getRenderBounds(bounds);

    GeometryGenerator::Line2dParameters lineParams;
    lineParams.lineWidth = worldBounds.height();
    lineParams.y0 = lineParams.y1 = worldBounds.getPosition(Bounds::center).y;
    lineParams.x0 = worldBounds.left();
    lineParams.x1 = worldBounds.right();

    GeometryGenerator::GeometryIndices body;
    generator->startShape();
    generator->generateLine2d(lineParams);
    generator->endShape(&body);

    resetShader();
    m_app->getShaders()->SetBaseColor(fillColor);
    m_app->drawGenerated(body, 0x11, m_app->getShaders()->GetUiFlags());
}

void UiElement::drawText(
        const std::string &s,
        const Bounds &bounds,
        float height,
        const Point &ref)
{
    const Bounds renderBounds = unitsToPixels(getRenderBounds(bounds));
    const Point origin = renderBounds.getPosition(ref);

    m_app->getTextRenderer()->RenderText(
            s, origin.x, origin.y - height / 4, height);
}

void UiElement::drawAlignedText(
        const std::string &s,
        const Bounds &bounds,
        float height,
        const Point &ref,
        const Point &refText)
{
    const Bounds renderBounds = unitsToPixels(getRenderBounds(bounds));
    const Point origin = renderBounds.getPosition(ref);

    const float textWidth = m_app->getTextRenderer()->CalculateWidth(s, height);
    const float textHeight = height;

    const Bounds textBounds(
        textWidth,
        textHeight,
        { 0.0f, textHeight - textHeight * 0.25f },
        Bounds::tl);
    const Point r = textBounds.getPosition(refText);

    m_app->getTextRenderer()->RenderText(
        s, origin.x - r.x, origin.y - r.y, height);
}

void UiElement::drawCenteredText(
        const std::string &s,
        const Bounds &bounds,
        float height,
        const Point &ref)
{
    const Bounds renderBounds = unitsToPixels(getRenderBounds(bounds));
    const Point origin = renderBounds.getPosition(ref);

    const float width = m_app->getTextRenderer()->CalculateWidth(s, height);
    m_app->getTextRenderer()->RenderText(
            s, origin.x - width / 2, origin.y - height / 4, height);
}
